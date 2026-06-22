# Provisioning & orchestration (the handoff build→deploy flow)

Stage 4 of the [pipeline](artheia-gen-system.md): once the `.art` is generated
and the FCs build, this is how a rig reaches a machine. It is **manifest-driven
and agentless (SSH/Ansible)**. Puppet is retired; `.ipk`/`.yaml` are gone — the
unit is the per-machine **`.deb`** and the manifests are **JSON**.

## The flow — three commands, in order

```sh
# 1. SERIALIZE the rig → per-machine JSON manifests + the dist BUILD glue.
theia manifest <rig>
#    writes dist/manifest/{machines.json, <machine>/{machine,execution,service,
#    application,executor}.json, BUILD.bazel}. The validate-before-serialize
#    gate runs FIRST (refuses on any deployment inconsistency).

# 2. DIST — cross-build the per-machine .deb from those manifests.
theia dist
#    builds //dist/manifest:<machine>_pkg (rules/dist_ipk.bzl), cross-compiled
#    for the machine.json arch, packed from execution.json. → dist/manifest/
#    <machine>/<machine>.deb

# 3. ORCHESTRATE — push + install the .deb, drop executor.json + config, reload.
theia orchestrate <machine>          # = ansible-playbook deploy/ansible/orchestrate.yml
#    provisioning (OS pkgs + etcd + Mender) is the one-time `theia provision`.
```

`theia provision`/`orchestrate` wrap `ansible-playbook` against
`deploy/ansible/{provision,orchestrate}.yml`, limiting to the host whose
inventory `machine=` matches. The inventory (machine→ssh host/user) is
`deploy/ansible/inventory/hosts` — the ONE deploy fact the manifest can't carry.

## ⚠ Footguns (these have bitten — read them)

- **Do NOT pass `theia manifest --out <dir>`** for a real deploy. The dist BUILD
  glue (`dist/manifest/BUILD.bazel`, which `theia dist` needs) is only emitted
  when the out-dir is the default `dist/manifest`; `--out` makes a *throwaway*
  serialize dir and the glue is skipped → `theia dist` fails with "no such
  package dist/manifest". Use the bare `theia manifest <rig>`.
- **`dist_pkg` binaries come from the manifest, not the rule default.** `theia
  manifest` emits `dist_pkg(binaries=[…])` derived from execution.json. The
  rule's fallback `ALL_BINARIES` hardcodes the demo apps (`@@//apps/Demo3WayP*`)
  — absent in a service-only / consuming workspace → analysis fails. A rig that
  has no apps (e.g. the rpi4 service-test rig) must `Remove()` them from BASE so
  they're not in execution.json (see manifest-py-syntax.md).
- **The workspace must contain the binaries the manifest references.** If a rig
  binds `p1..p4` but the demo apps were never `gen-app`'d here, `theia dist`
  can't build them. Either gen them (`artheia gen-app --kind fc
  demo/system/apps/component.art --out apps`) or use a rig that doesn't include
  them.
- **Ad-hoc binary testing ≠ the deploy flow.** For a one-off "does this binary
  run on the Pi" check, just `scp bazel-out/.../bin <host>:` and launch it with
  `THEIA_SUPERVISOR_MANIFEST=<executor.json>` (+ `THEIA_CONFIG_DIR`,
  `THEIA_TIPC_SCOPE`). Don't stand up a parallel playbook.

## Cross-compile note (rpi4 / aarch64)

`theia dist` reads `machine.json` `arch` and cross-builds with
`--platforms=//rules/config:rpi4` against `third_party/sysroot/rpi4` (built by
`setup_rpi4.sh`). The `bazel-bin` symlink points at the LAST build's arch — run
`file -L bazel-bin/.../<bin>` to confirm aarch64 before staging by hand.

## Mental model

- The **`.deb` is the deploy unit** — one per machine, `dpkg -i`'d under
  `/opt/theia/` by orchestrate.yml.
- **`executor.json` is the supervisor's input** — the supervision tree for this
  host (`THEIA_SUPERVISOR_MANIFEST`).
- **`config/<fc>.json`** are the per-FC static params (`THEIA_CONFIG_DIR`).
- Ansible only lays files down; the **supervisor** starts, watches, and reloads
  FC processes (a `theia orchestrate` re-push notifies a reload, no downtime).

See [manifest-py-syntax.md](manifest-py-syntax.md) for how a rig.py composes the
deployment (the `Append`/`Remove` monoid algebra).

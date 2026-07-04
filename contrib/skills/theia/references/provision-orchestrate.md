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

## Fleet path — the colony web API (S3-exclusive, GS-driven)

The `theia orchestrate <machine>` CLI above is the LOCAL/dev handoff (reads the
local `dist/manifest`). The FLEET path is **colony**: a separate S3-exclusive
deploy service (repo `colony`, runs as the `colony-api` container behind the
Ground Station) that the GS calls to provision/orchestrate/cleanup real devices.
It is **registry-free** — every deploy fact comes from the request, and the
per-role runtime + manifest come from the **S3 runtime plane**
(`s3://theia-runtime/<version>/`), NOT the local tree.

### Two-step model + roles

- **provision** — one-time board setup: base dirs, OS packages, etcd, TIPC
  bearer, the Mender client + the theia OTA update modules
  (theia-swp/theia-app/theia-release) + state-scripts.
- **orchestrate** — per-deploy: pull the runtime+services `.deb`s from S3,
  `dpkg -i`, lay the release-dir (`current → releases/<ver>`), push config, reload.
- **cleanup** — the inverse (stop supervisor, remove /opt/theia, dpkg -r).

The framework services manifest is **role-keyed**: exactly two machines whose
name IS the role — `master` (full platform, etcd, all singletons) and `zonal`
(the worker slice: shwa+ucm). `1 master, N zonal, all named zonal`; N-arity is a
DEPLOY fact (colony fans the one zonal slice onto N boards), the framework names
no boards. **Always pass `role`** so colony pulls the right `manifest/<role>/`
slice.

### The HTTP API

`colony-api` listens on `:8081` inside the GS docker network (not exposed to the
host). Endpoints:

```
POST /deployments               {rig, kind, host, extra} → {id, status, …}
GET  /deployments               the run journal
GET  /deployments/{id}          one deployment's status + statistics
GET  /deployments/{id}/log      the Ansible output
POST /deployments/{id}/abort    abort a pending/scheduled run
```

`kind ∈ {provision, orchestrate, cleanup}`. `host = "ip[:port]"` (a docker test
rig is a container on the host net at a distinct SSH port). `role`,
`runtime_version`, `machine_instance`, `etcd_external`, cleanup scope flags ride
in **`extra`** (colony pops `role` → `--role`, the rest → `-e k=v`).

```jsonc
// provision central as the master role
{"rig":"central","kind":"provision","host":"10.0.0.23:2201",
 "extra":{"role":"master","machine_instance":"0",
          "runtime_version":"0.2.2-amd64","etcd_external":"true"}}
```

Poll `GET /deployments/{id}` until `status` is `finished`/`failed`; success =
`statistics.status.success > 0 and failure == 0`.

Reach it from a host on the GS network (the `gs-api`/`colony-api` containers) —
e.g. `docker exec gs-api python3` with `urllib` against `http://colony-api:8081`.
The MCP `colony_deploy`/`colony_deployment_status`/`colony_deployment_log` tools
wrap this API when the endpoint is reachable.

### ⚠ colony footguns (these have bitten)

- **`ansible_connection` inheritance.** colony resolves the target in a
  `hosts: localhost` play then `add_host`s the board. The add_host must read a
  DEDICATED `target_connection` var (default `ssh`) — NOT `ansible_connection`,
  which is already `local` in the localhost play and would make every board task
  run ON THE CONTROLLER (copies land in colony-api's own filesystem, deploys
  silently no-op). Docker-test-rig path overrides with
  `-e target_connection=community.docker.docker`.
- **`role` reporting needs the inline `machine_index`.** `machines.json` must
  carry `machine_index: {master:0, zonal:1}` (the serializer emits it) — com's
  MachineManifest keys the `rtdb machines` ROLE column off it. A stale S3
  manifest without it → ROLE shows `—`. Fix = republish the runtime plane.
- **Safe-base HW gating.** The serialized manifest carries
  `deploy/config/master/executor.json` (`run_on_start:false` for fw/tsync/rds)
  so the runtime deploys cleanly on ANY rig regardless of HW/CAPA — without it a
  bare rig (docker, minimal board) crash-loops those FCs and escalates the whole
  supervisor tree. A user SWP inherits this (it inherits the rig).

### Local docker rig (the dev fleet)

`deploy/docker-compose.yml` runs `theia-{central,compute,frontal}` + a
`theia-etcd` sidecar (`theia rig up`) as `network_mode: host` SSH rigs
(:2201/:2202/:2203, all `axadmin@<host-ip>`), which colony provisions over SSH —
the local mirror of a real fleet. The rig entrypoint starts sshd + the Mender
agent, and polls for `/opt/theia/current` to start the supervisor once
orchestration lands it.

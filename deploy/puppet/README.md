# Theia deploy — Puppet manifests

Two-phase deploy model with strict separation:

| Phase | Entry | Reads | Touches | When |
|---|---|---|---|---|
| **Provisioning** | `provisioning.pp` → `theia::provisioning` | `/etc/theia/manifest/machine.json` | OS packages, opkg artifacts (supervisor, gateway), systemd units, setcap; (deferred) `services/per` migration | greenfield install; full update including supervisor / gateway; schema-version bumps |
| **Orchestration** | `orchestration.pp` → `theia::orchestration` | `/etc/theia/manifest/application.json` | App `.ipk`s under `/opt/theia/apps/`; supervisor reload signal (`tdb reload`) | day-to-day app pushes |

Both phases derive the per-machine input from
`dist/manifest/<machine>/{machine,application}.json`, which is what
`artheia generate-manifest` writes (JSON; the supervisor + tooling
parse JSON). The full layout of the per-machine manifest dir:

```
/etc/theia/manifest/
├── machine.json          ← os_packages + opkg_artifacts (Phase 1)
├── application.json      ← AAs + their .ipks (Phase 2)
├── service.json          ← SOA bindings (read at runtime)
└── execution.json        ← Process list + supervisor tree slice
```

## Invocation

Use the `theia` wrapper (it points `--hiera_config` at
`deploy/puppet/hiera.yaml` and resolves the modulepath):

```bash
THEIA_MACHINE=central_host theia provision     # Phase 1
THEIA_MACHINE=central_host theia orchestrate    # Phase 2
```

…or call `puppet apply` directly:

```bash
THEIA_MACHINE=central_host \
    puppet apply --modulepath=deploy/puppet/modules \
                 --hiera_config=deploy/puppet/hiera.yaml \
                 deploy/puppet/provisioning.pp
```

`$THEIA_MACHINE` lets us drive Puppet against a specific machine
identity without renaming the host. Fall-through is `$facts['hostname']`.

## Two-phase rationale

Day-to-day app pushes happen **often** (any merged feature). They
must NOT trigger:

- supervisor / gateway downtime
- `services/per` schema migration
- systemd unit reloads beyond the affected app

A full update — supervisor binary changes, gateway changes, schema
bump — is much **rarer** and inherently disruptive: the supervisor
must restart, `services/per` may need offline migration, and the
rollback story is "snapshot + reprovision". Splitting these into two
phases lets us run Phase 2 from a CI-driven push pipeline without
operator intervention, and reserves Phase 1 for change-controlled
releases.

## Module classes — real vs deferred

The site `.pp` files (`provisioning.pp`, `orchestration.pp`) and the
per-machine entries (`manifests/central.pp`, `manifests/compute.pp`)
are wired in. The module classes are now REAL:

- `theia::provisioning` — `parsejson(machine.json)` → `package{}` per
  `os_packages`, `dpkg`-installs each `opkg_artifacts` .ipk, drops a
  systemd unit (`templates/theia-unit.service.epp`), enables the
  service, then applies the setcap contract (`theia::postinstall`).
- `theia::orchestration` — `parsejson(application.json)` → `package{}`
  per buildable component .ipk under `/opt/theia/apps/`, notifying
  `tdb reload` on a version change (no restart).

**Deferred** (single marker, not a stub of the whole class):

- `services/per` offline schema migration in Phase 1 — waits on a
  `tdb migrate --check/--apply` CLI. Until then a `notice(...)` marks
  the step.

## Cross-references

- `docs/autosar/manifest.md` — the four AUTOSAR manifest kinds
  (machine / application / service / execution) we serialize from
  rig.py.
- `docs/tasks/DONE/02-puppet-provisioning-orchestration.md` —
  history of this task and the design decisions.
- `docs/skills/theia/references/migration.md` — `services/per`
  config-schema migration (the deferred Phase 1 step).

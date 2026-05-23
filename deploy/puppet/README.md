# Theia deploy — Puppet manifests

Two-phase deploy model with strict separation:

| Phase | Entry | Reads | Touches | When |
|---|---|---|---|---|
| **Provisioning** | `provisioning.pp` → `theia::provisioning` | `/etc/theia/manifest/machine.yaml` | OS packages, opkg artifacts (supervisor, gateway, etcd), systemd units, `services/db` migration | greenfield install; full update including supervisor / gateway / etcd; schema-version bumps |
| **Orchestration** | `orchestration.pp` → `theia::orchestration` | `/etc/theia/manifest/application.yaml` | App `.ipk`s under `/opt/theia/apps/`; supervisor reload signal | day-to-day app pushes |

Both phases derive the per-machine input from
`dist/manifest/<machine>/{machine,application}.yaml`, which is what
`artheia generate-manifest` writes. The full layout of the per-
machine manifest dir:

```
/etc/theia/manifest/
├── machine.yaml          ← os_packages + opkg_artifacts (Phase 1)
├── application.yaml      ← AAs + their .ipks (Phase 2)
├── service.yaml          ← SOA bindings (read at runtime)
└── execution.yaml        ← Process list + supervisor tree slice
```

## Invocation

```bash
# Phase 1: provisioning (first install or major update).
THEIA_MACHINE=central_host \
    puppet apply --modulepath=deploy/puppet/modules \
                 deploy/puppet/provisioning.pp

# Phase 2: orchestration (app pushes, no infra change).
THEIA_MACHINE=central_host \
    puppet apply --modulepath=deploy/puppet/modules \
                 deploy/puppet/orchestration.pp
```

`$THEIA_MACHINE` lets us drive Puppet against a specific machine
identity without renaming the host. Fall-through is `$facts['hostname']`.

## Two-phase rationale

Day-to-day app pushes happen **often** (any merged feature). They
must NOT trigger:

- supervisor / gateway / etcd downtime
- `services/db` migration
- systemd unit reloads beyond the affected app

A full update — supervisor binary changes, gateway changes, etcd
schema bump — is much **rarer** and inherently disruptive: the
supervisor must restart, `services/db` may need offline migration,
and the rollback story is "snapshot + reprovision". Splitting these
into two phases lets us run Phase 2 from a CI-driven push pipeline
without operator intervention, and reserves Phase 1 for change-
controlled releases.

## What's a STUB today vs production-ready

The site `.pp` files (`provisioning.pp`, `orchestration.pp`) and the
existing per-machine entries (`manifests/central.pp`,
`manifests/compute.pp`) are wired in correctly.

The **module classes** (`modules/theia/manifests/{provisioning,orchestration}.pp`)
are intentionally STUB: they `notice(...)` what they would do and
the production-shape code lives in commented-out blocks marked
`STUB`. The blocks become real once:

- `dist/manifest/<machine>/` lands inside the container's
  `/etc/theia/manifest/` (today docker-compose bind-mounts the raw
  rig output; Distribution tarball lands later).
- `bazel build //platform/{supervisor,gateway}:ipk` produces real
  `.ipk` files (today both are bash-stub placeholders — see
  `demo/BUILD.bazel`'s `_STUB_CMD`).
- `services/db` exists and `services-db migrate --check` returns
  meaningful exit codes (today: the entire DB layer is a BACKLOG
  item).

Once those land, replace the `notice(...)` markers with the
production-shape Puppet code in the same files (it's already there
as comments).

## Cross-references

- `docs/autosar/manifest.md` — the four AUTOSAR manifest kinds
  (machine / application / service / execution) we serialize from
  rig.py.
- `docs/tasks/DONE/02-puppet-provisioning-orchestration.md` —
  history of this task and the design decisions.
- `artheia/generators/dist_manifest.py` — the emitter that writes
  the per-machine YAML files this Puppet flow consumes.
- `docs/tasks/BACKLOG/etcd-state-backbone.md` — context on
  `services/db` and why migration is a Phase 1 concern.

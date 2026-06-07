# Puppet provision/orchestrate — machine-generic, manifest-driven

The deploy lifecycle, unified around puppet as the always-on convergence agent.

## Roles

- **provision** (Phase 1, rare): install **etcd ONLY** — on the one-per-cluster
  machine whose `machine.json` lists `etcd-server` (central). Everything else's
  provision is a no-op. Drop the EMPTY executor.json (root node, no children) so
  the supervisor can boot. This is the cheap, stable base.
- **orchestrate** (Phase 2, frequent): install the per-machine `.ipk` (supervisor
  + FC/app binaries) + the REAL configs (execution.json.supervisor_tree →
  executor.json, machines.json, params). Then the supervisor runs the real tree.

Both are **machine-generic**: take `$machine` (from `FACTER_theia_machine`), read
`<machine>/{machine,application,execution}.json`. NO per-host .pp files
(central.pp/compute.pp are dropped).

## Who runs puppet (always-on convergence)

| environment | puppet driver |
| --- | --- |
| docker (fresh boot) | `run-supervisor.sh`: puppet provision → orchestrate → then supervise. If the supervisor fails, FALL BACK to puppet (re-converge). |
| real host | systemd runs puppet (timer/service) — always on, handles upgrades. |

So `run-supervisor.sh` is no longer a dumb wait loop; it is the in-container
puppet driver + supervisor runner + fallback.

## Flow (docker)

```
docker compose up
  ├─ etcd service starts (one per cluster)
  └─ central/compute entrypoint = run-supervisor.sh:
       1. puppet apply theia::provisioning   # etcd-host? install etcd. empty executor.json.
       2. puppet apply theia::orchestration   # install <machine>.ipk + real configs
       3. exec /opt/theia/bin/supervisor  (real tree)
            └─ on failure → goto puppet (re-converge), don't just die
```

Checkpoint after provision: supervisor boots on the empty tree (`tdb ps` = bare
root) on every machine. After orchestrate: the real tree runs.

## What changes

1. **zonal_rig.py** — etcd-server only on central (DONE). machine.json drives it.
2. **docker-compose** — add an `etcd` service (one per cluster); central
   depends_on it; THEIA_ETCD_EXTERNAL so provision skips in-container etcd
   install (DONE). compute has no etcd.
3. **run-supervisor.sh** — regain puppet: provision → orchestrate → supervise,
   with supervisor-failure → puppet fallback. (Reverses the "no puppet inside"
   gutting, but now it's the deliberate convergence driver.)
4. **theia::provisioning** — slim to: etcd (manifest-driven, skip if external) +
   empty executor.json. Drop the .ipk/systemd-unit/setcap work → that moves to
   orchestrate (or stays minimal). Machine-generic ($machine param).
5. **theia::orchestration** — install <machine>.ipk (supervisor + apps) + real
   executor.json (from execution.json.supervisor_tree) + machines.json + setcap.
   Machine-generic.
6. **Drop central.pp / compute.pp** — provision/orchestrate read $machine from
   FACTER_theia_machine; no per-host site manifests.
7. **theia provision / theia orchestrate** (theia.py) — host-side verbs that
   `puppet apply` the machine-generic class with FACTER_theia_machine=<m>.

## Open / to confirm during impl

- Exact "supervisor failed → fallback to puppet" loop shape in run-supervisor.sh
  (retry count? re-run orchestrate then re-exec?).
- Does provision really do ONLY etcd + empty executor (lighter than the current
  provisioning.pp which also installs the bundle)? → per user: provision=etcd,
  orchestrate=ipkg+configs. Move the bundle install to orchestrate.
- machines.json (RigIndex) vs per-machine: orchestrate reads <machine>/*; the
  machine list comes from the top-level machines.json.

## Status

Model locked (puppet = always-on convergence; provision=etcd+empty-executor,
orchestrate=ipk+real-config; machine-generic, no per-host .pp). Steps 1–2 DONE
(rig etcd-central, compose etcd service). 3–7 to implement.

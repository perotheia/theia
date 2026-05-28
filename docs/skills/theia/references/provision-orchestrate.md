# Provisioning & orchestration

Stage 4 of the [pipeline](artheia-gen-system.md). Once Bazel has produced
per-machine `.ipk` bundles + `executor.yaml` + `machines.yaml`, Puppet
(or `tools/deploy_rpi4.sh` for bare metal) installs them and brings the
stack up. There are two real paths today: the **Docker compose dev rig**
(end-to-end working) and the **bare-metal Pi 4 push** (working for the
gateway). The bare-metal Puppet body for production targets is still
aspirational scaffolding.

## The two-phase Puppet model

`deploy/puppet/` splits provisioning by frequency and blast radius:

| manifest | when to use | what it touches |
| --- | --- | --- |
| `provisioning.pp` | greenfield install, supervisor / gateway / etcd bump, schema-delta release | OS packages, supervisor + gateway `.ipk`, `executor.yaml`, restarts everything |
| `orchestration.pp` | day-to-day **application** pushes | re-installs `.ipk`s under `/opt/theia/apps/` only — supervisor reloads its child table after the swap |

Both resolve the target via `$THEIA_MACHINE` env (falling back to
`$facts['hostname']`) and drive the `theia` module in
`deploy/puppet/modules/`.

```sh
# greenfield / full update
puppet apply --modulepath=deploy/puppet/modules \
             deploy/puppet/provisioning.pp \
             --hiera_config=/dev/null

# app-only push (no service downtime)
puppet apply --modulepath=deploy/puppet/modules \
             deploy/puppet/orchestration.pp \
             --hiera_config=/dev/null
```

The supervisor's child reload after an `orchestration.pp` apply is what
makes it safe on a live system — but it relies on the new `.ipk` having
the same FC catalog as the previous one. A schema-delta release goes
through `provisioning.pp`.

## The Docker compose dev rig — end to end

`deploy/docker-compose.yml` brings up Theia as two containers (`central`
+ `compute`) on a shared bridge network — matching `demo/manifest/rig.py`'s
two-machine split. Quick start (from the repo root, with the workspace
venv on PATH):

```sh
# 1. Build the supervisor (still CMake, not Bazel-targeted yet).
cd platform/supervisor && cmake -S . -B build && cmake --build build -j
cd ../..

# 2. Build the per-machine .ipks + the two YAMLs.
bazel build @rig_demo//:all

# 3. Stage Bazel output into deploy/.staging/ (docker-compose bind-mounts it).
./deploy/stage.sh                       # default rig = rig_demo

# 4. Build the per-host Docker images.
docker compose -f deploy/docker-compose.yml build

# 5. Bring it up.
docker compose -f deploy/docker-compose.yml up    # Ctrl-C to stop
```

Each container's entrypoint is `deploy/run-supervisor.sh`:

1. Picks `/etc/puppet/manifests/$HOSTNAME.pp` (`central.pp` or
   `compute.pp`).
2. `puppet apply` runs the `theia` module — `theia::install`
   (`opkg install /opt/theia/ipk/<machine>.ipk`), `theia::config`
   (drops `executor.yaml` + `machines.yaml` into `/etc/theia/`),
   `theia::service` (placeholder — supervisor runs foreground in the
   dev compose).
3. Verifies `/usr/bin/theia-supervisor` exists + `/etc/theia/executor.yaml`
   is present.
4. `exec`s the supervisor in foreground so Docker signals
   (`docker stop` → SIGTERM) reach it directly for graceful shutdown.

Port map: GUI talks to **`host:7700`** for central, **`host:7701`** for
compute (both inside the bridge network at `172.30.0.0/24`).

## Bare-metal push (Pi 4, gateway)

`tools/deploy_rpi4.sh` is the working bare-metal path — cross-build,
`scp`, `dpkg -i`, with a TIPC `modprobe` sanity check on the target.
Use it for one-shot pushes to a Pi 4 dev rig; production targets go
through Puppet once the modulepath body lands.

## Mental model

- The **`.ipk` is the deploy unit** — one per machine, opkg'd in by
  Puppet under `/opt/theia/`.
- The **`executor.yaml` is the supervisor's input** — the
  `service`/`execution` slice of the rig for this host.
- The **`machines.yaml` is the GUI's input** — per-machine gRPC
  endpoints, nothing more.
- Puppet only lays files down; the **supervisor** is the one that
  actually starts, watches, and reloads FC processes.

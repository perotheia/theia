# deploy/ — multi-host Theia bringup in Docker

Brings up Theia as two containers (`central` + `compute`) on a
shared Docker bridge network. Each runs its own supervisor;
together they realize the two-machine topology described in
`demo/manifest/rig.py::DemoSoftware`.

## Topology

```
   host
  ┌─────────────────────────────────────────────────────────────┐
  │                                                             │
  │   ┌──────────────────┐         ┌──────────────────┐         │
  │   │  theia-central   │         │  theia-compute   │         │
  │   │  hostname=central│         │  hostname=compute│         │
  │   │  alias=central_  │         │  alias=compute_  │         │
  │   │        host      │         │        host      │         │
  │   │                  │         │                  │         │
  │   │ /usr/bin/theia-  │         │ /usr/bin/theia-  │         │
  │   │   supervisor     │         │   supervisor     │         │
  │   │ /usr/bin/{18 FCs}│ ◄─TIPC─►│ /usr/bin/demo_p1 │         │
  │   │ /usr/bin/gateway │         │ /usr/bin/demo_p2 │         │
  │   │                  │         │ /usr/bin/demo_p3 │         │
  │   │ gRPC :7700       │         │ gRPC :7701       │         │
  │   └────────┬─────────┘         └────────┬─────────┘         │
  │            │                            │                   │
  │            └────────── theia_net ───────┘                   │
  │                       (172.30.0.0/24)                       │
  └─────────────┬──────────────────────────────────────┬────────┘
                │                                      │
            host:7700                              host:7701
            (GUI → central)                       (GUI → compute)
```

## Lifecycle

Each container's lifecycle:

1. Docker spawns the container with the entrypoint
   `/usr/local/bin/run-supervisor.sh`.
2. The script picks the Puppet manifest matching its `$HOSTNAME`
   (`/etc/puppet/manifests/central.pp` or `compute.pp`).
3. `puppet apply` runs the `theia` module:
   - `theia::install` — `opkg install /opt/theia/ipk/<machine>.ipk`.
   - `theia::config` — drops `executor.yaml` + `machines.yaml`
     into `/etc/theia/`.
   - `theia::service` — placeholder (in the dev compose, the
     supervisor runs in foreground after Puppet returns).
4. The script verifies `/usr/bin/theia-supervisor` exists and
   `/etc/theia/executor.yaml` is present.
5. `exec`s the supervisor binary in foreground (Docker signals
   reach the supervisor directly; `docker stop` → SIGTERM →
   graceful shutdown).

## Quick-start

```bash
# 1. Install the workspace venv.
python3 -m venv .venv
.venv/bin/pip install -e ./artheia
export PATH="$PWD/.venv/bin:$PATH"

# 2. Build the supervisor binary (CMake — not yet a Bazel target).
cd platform/supervisor && cmake -S . -B build && cmake --build build -j
cd ../..

# 3. Build the per-machine .ipks + the two YAMLs.
PATH="$PWD/.venv/bin:$PATH" bazel build @rig_demo//:all

# 4. Stage artifacts into deploy/.staging/.
./deploy/stage.sh

# 5. Build the base Docker image + per-host images.
docker compose -f deploy/docker-compose.yml build

# 6. Bring it up (Ctrl-C to stop).
docker compose -f deploy/docker-compose.yml up
```

After `up`:

- `docker logs -f theia-central` — central's supervisor + Puppet apply
- `docker logs -f theia-compute` — compute's
- GUI: connect to `localhost:7700` (central) and `localhost:7701`
  (compute) from the host.

## Files

| File | Role |
|---|---|
| `Dockerfile.base`           | Ubuntu + opkg + puppet + bash. Shared base image. |
| `central/Dockerfile`        | Sets HOSTNAME=central, exposes 7700. |
| `compute/Dockerfile`        | Sets HOSTNAME=compute, exposes 7701. |
| `run-supervisor.sh`         | Container entrypoint (`puppet apply` then `exec supervisor`). |
| `docker-compose.yml`        | Two services + shared bridge network. |
| `stage.sh`                  | Copies Bazel artifacts to `.staging/` for compose to mount. |
| `puppet/central.pp`         | Per-host manifest: include the theia module with machine="central_host". |
| `puppet/compute.pp`         | Same, machine="compute_host". |
| `puppet/modules/theia/manifests/init.pp`    | Aggregator class. |
| `puppet/modules/theia/manifests/install.pp` | `opkg install <ipk>`. |
| `puppet/modules/theia/manifests/config.pp`  | Drop executor.yaml + machines.yaml. |
| `puppet/modules/theia/manifests/service.pp` | Lifecycle (placeholder; entrypoint handles it). |
| `.staging/` (gitignored)    | Staged Bazel artifacts. Populated by `stage.sh`. |
| `logs/` (gitignored)        | Container stdout/stderr captures. |

## Known limits

- **Central image's .ipk is empty today**. The 18 FC SwComponents
  are `bazel_buildable=False` (their binaries are bash daemons
  under `theia_runtime/`, not yet bridged into Bazel). So
  `@rig_demo//central_host:image` is an empty filegroup; opkg-install
  becomes a no-op. The supervisor runs but has no FC binaries to
  spawn — it logs "module missing" for each.

  **Fix path**: bridge `theia_runtime/services/<short>/daemon.sh` →
  cc_binary (or sh_binary) targets, flip `bazel_buildable=True` in
  `services/manifest/fc.py`.

- **Supervisor binary is bind-mounted, not installed via opkg**.
  Until packaged as part of an .ipk, we mount it from the
  workspace build. See `docker-compose.yml`'s
  `../platform/supervisor/build/supervisor:/usr/bin/theia-supervisor`.

- **No systemd in the dev compose**. The entrypoint exec's the
  supervisor in foreground. Production deploys with
  `--privileged` + systemd as PID 1 are a future task; the Puppet
  `theia::service` class has the systemd unit code commented out
  ready to enable.

- **Cross-container TIPC not yet plumbed**. The demo's TIPC
  multicast doesn't traverse Docker bridges out of the box; the
  current rig is designed for in-host TIPC. For real
  multi-container TIPC, either run with `--network=host` or
  configure `tipc-config link` to span the Docker bridge.
  Tracked in `docs/tasks/TODO/cross-container-tipc.md`.

## Troubleshooting

**`puppet apply` errors with "no such file or directory:
/opt/theia/ipk/<machine>.ipk"**
You forgot `./deploy/stage.sh`. The compose mount expects the
.ipk to exist under `.staging/<machine>/ipk/`.

**Container exits immediately with rc=3**
The supervisor binary isn't at `/usr/bin/theia-supervisor`. Check
the bind-mount path in `docker-compose.yml` — the source is
`../platform/supervisor/build/supervisor`, which only exists after
`cmake --build build` runs successfully.

**Both containers up but supervisor logs "service missing"**
The FC binaries aren't installed. Expected today; see "Known
limits" above.

**Want to inspect the .ipk before deploy**
```bash
ar t deploy/.staging/compute/ipk/compute_host.ipk     # list members
ar x deploy/.staging/compute/ipk/compute_host.ipk      # extract
tar -tzf data.tar.gz                                   # see /usr/bin/* entries
```

## Related docs

- [Bazel integration](../docs/artheia/bazel.md) — how `bazel build`
  drives the per-machine `.ipk`s.
- [Manifest DSL](../docs/artheia/manifest-dsl.md) — how the rig.py
  describes machine bindings.
- [Tutorial: gen-rig](../docs/artheia/gen-rig.md) — bootstrapping
  a new rig.py.

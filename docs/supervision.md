# Process supervision

The runtime hosts ~25 daemons on each machine: the 18 Adaptive Platform
Functional Clusters plus a per-vehicle set of vendor applications. They
crash. The supervisor is the component that decides what to restart
and in what order when one of them does.

We model supervision on Erlang/OTP. The OTP supervisor pattern has
been doing this job since the late 1990s on telecom hardware that
explicitly accepts that processes will crash; copying it costs us
nothing and skips re-discovering its failure modes. References:

- [OTP design principles — supervisors](https://erlang.org/documentation/doc-4.9.1/doc/design_principles/sup_princ.html)
- [supervisor module reference](https://www.erlang.org/docs/20/man/supervisor)

This document covers the model, the data shapes, the strategies, the
manifest format, and how it composes from the artheia rig.

---

## Model

A **supervisor** owns a set of **children**. Each child is either a
**worker** (a leaf — one POSIX process) or another **supervisor**
(internal bookkeeping; not a forked process). When a worker exits, its
direct parent supervisor observes the exit, classifies it as normal or
abnormal, and applies its **restart strategy** to decide what to
restart.

If the supervisor restarts children too often inside a bounded time
window, it gives up and **escalates** — terminates everything in its
subtree and lets *its* parent decide. The root supervisor's escalation
exits the binary.

### Restart strategies

| Strategy | Trigger | Effect |
|---|---|---|
| `one_for_one` | child X exits | restart X |
| `one_for_all` | any child exits | terminate every child, restart every child |
| `rest_for_one` | child X exits | terminate X and every child *declared after* X; restart them in order |
| `simple_one_for_one` | child X exits | like `one_for_one`, but children are added dynamically from one template |

`simple_one_for_one` is included for spec completeness; it is logged
but not exercised because we don't add children at runtime today.

### Restart types

Per-child, *whether* a restart fires at all:

| Type | Restarts when |
|---|---|
| `permanent` | always |
| `transient` | only on abnormal exit (non-zero / signal-killed) |
| `temporary` | never |

### Bounded restart intensity

Every supervisor carries `max_restarts` and `max_seconds`. The
supervisor maintains a sliding window of restart timestamps for itself;
if `len > max_restarts` within `max_seconds`, escalation fires. OTP
defaults are `1/5s` for the root; we use `3/5s` for `rest_for_one`
trees that can have a few crashes settle before declaring the system
broken.

### Shutdown

When a supervisor terminates a child it sends SIGTERM and waits
`shutdown` milliseconds before SIGKILL. Two reserved values:

- `"brutal_kill"` — immediate SIGKILL, no SIGTERM.
- `"infinity"` — wait forever (use for supervisors, never for workers
  that might wedge).

---

## Data shapes

Authored in Python (see `artheia/armanifest/`), serialized to YAML,
executed by the C++ supervisor binary.

### `ChildSpec` (one leaf)

```python
ChildSpec(
    name="core",                       # unique within parent supervisor
    start_cmd=["services/core/daemon.sh"],
    restart=RestartType.PERMANENT,     # permanent | transient | temporary
    shutdown=5000,                     # ms | "brutal_kill" | "infinity"
    type=ChildType.WORKER,             # worker | supervisor
    modules=["services/core"],         # informational, like OTP's modules list
    env={},                            # extra env vars
    working_dir="",                    # cwd override
)
```

### `SupervisorSpec` (an internal node)

```python
SupervisorSpec(
    name="services",
    strategy=RestartStrategy.REST_FOR_ONE,
    max_restarts=3,
    max_seconds=5,
    children=[ChildSpec(...), SupervisorSpec(...), ...],
)
```

The two are distinguishable in YAML by the presence of `children` —
that's all the supervisor binary needs to know.

---

## Dependency derivation

Static dependency information lives in `.art` files at
`platforms/system/services/<short>/package.art`. Each FC's atomic node
declares the interface it **provides** (server port) and the
interfaces it **requires** (client ports). A topological sort of
these edges yields a start order: `core` first, leaf FCs last.

`build_supervisor_tree(rig)` in `artheia.armanifest.supervisor`:

1. Walks `platforms/system/services/<short>/package.art` via the
   artheia textX parser.
2. For each FC, records its provided interface and the interfaces it
   requires. Resolves required→provider FC short-names.
3. Kahn topological sort yields the start order.
4. Builds one `ChildSpec` per FC (start_cmd points at the daemon).
5. Builds one `ChildSpec` per vendor `SwComponent` (start_cmd points
   at the app's daemon).
6. Composes the root supervisor with the FCs first, the apps
   sub-supervisor last, all under `rest_for_one`.

The current dependency tiers (T0 first):

```
T0  core
T1  osi, log, tsync, exec, shwa
T2  crypto, per, com, phm
T3  nm, idsm, diag, sm, ucm, rds
T4  vucm, fw
```

---

## Tornado topology

```
root  (rest_for_one, 3/5s)
├── core
├── exec, log, osi, tsync, shwa         ← T1
├── crypto, per, com, phm               ← T2
├── nm, idsm, diag, sm, ucm, rds        ← T3
├── vucm, fw                            ← T4   (fw + shwa dropped by Macan layer)
└── apps  (one_for_one)
    ├── camera_viewer
    ├── mosaic_cluster
    ├── mosaic_sentry
    ├── unified_boot
    ├── unreal_camera_preview
    ├── interior_climate_manager
    ├── climate_arbitrator
    ├── mock_location_provider
    └── signal_probe_server
```

What this gives us:

- **`core` dies** → `rest_for_one` at the root restarts core and every
  child declared after it. Every FC and every vendor app cycles, in
  order. Apps come back last.
- **`com` (T2) dies** → restart com and every later child (nm/idsm/
  diag/sm/ucm/rds/vucm/fw + apps subtree). `core` and the T1 FCs
  stay up.
- **A single vendor app dies** → the inner `apps` supervisor uses
  `one_for_one`; only that app restarts. Services tree is undisturbed.

---

## CLI

### Emitting the manifest

```sh
artheia executor emit vendor.vehicles.tornado.arsyscomp --out executor.yaml
```

Loads the named Rig, calls `build_supervisor_tree`, serializes to YAML.

### Running the supervisor

```sh
./services/supervisor/build/supervisor run executor.yaml [--root-dir DIR]
```

The C++ binary is the only implementation. The spec — strategies,
restart types, shutdown semantics, escalation, tombstone surfacing —
lives in this document and is exercised by the test suite under
`services/supervisor/`. (A Python reference implementation existed
through mid-2026 and was removed once the C++ caught up on every
semantics row plus exclusively gained the TIPC publishing the
supervisor-gui consumes. Two parallel implementations were a
divergence trap.)

### Inspecting

```sh
# Find live children of a supervisor.
pstree -p $(pgrep -f 'supervisor run') | head

# Kill one to watch the cascade.
kill -9 $(pgrep -f services/core/daemon.sh)
```

---

## executor.yaml format

```yaml
name: root
strategy: rest_for_one
max_restarts: 3
max_seconds: 5
children:
- name: core
  start_cmd:
  - services/core/daemon.sh
  restart: permanent
  shutdown: 5000
  type: worker
  modules:
  - services/core
- name: exec
  start_cmd: [services/exec/daemon.sh]
  restart: permanent
  shutdown: 5000
  type: worker
# ...one entry per FC...
- name: apps
  strategy: one_for_one
  max_restarts: 3
  max_seconds: 5
  children:
  - name: camera_viewer
    start_cmd: [vendor/apps/camera_viewer/daemon.sh]
    restart: permanent
    shutdown: 5000
    type: worker
  # ...
```

Supervisors are distinguished from workers by the presence of
`children`. Field meanings match the dataclass docs above.

---

## Implementation notes

### Process group isolation

The supervisor forks each child with `setsid()` so it leads its own
process group. SIGTERM/SIGKILL is delivered to the *group*, not just
the leaf, so a daemon that itself spawned helpers (a wrapper script
running a real binary, say) tears down cleanly.

### Exit observation

The supervisor sleeps in `select()` on a `signalfd`. SIGCHLD interrupts
the sleep deterministically, the supervisor calls
`waitpid(-1, WNOHANG)` in a loop to reap all available exits, then
classifies and restarts as needed. No polling, no per-child threads.

### Reverse-order shutdown

When a supervisor stops a subtree (on shutdown or strategy-driven
restart), it stops children in **reverse declared order** before
restarting them in forward order. The declared order is the
dependency order; reverse-stop means dependents go down before what
they depend on.

### What this is not

- Not an init system. It supervises the application tree; the host's
  init manages the supervisor.
- Not a process manager with HTTP control. CLI + signals only.
- Not opinionated about logging. Children write to stdout/stderr;
  redirect at the supervisor's stdout (systemd, journald, file).

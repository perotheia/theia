# Per-app supervisor DSL — design

## Status

Phase B of the per-app-supervisor work. **Design proposal only — no
code changes yet.** Companion files in this directory:

- `README.md` — original task ticket (what the work delivers).
- `today-executor.yaml` — the executor.yaml shipping today (from
  `deploy/.staging/<m>/ipk/`, identical for both machines).
- this file — the Python DSL design.

## What stays vs what changes

### Stays

- **`SupervisorNode` is the DSL element.** Declared in Python
  (`artheia/manifest/supervisor.py`), wired through `Rig.supervisors`
  / `Layer.add_supervisors`, materialized via
  `build_supervisor_tree(rig)` → `SupervisorSpec` → emitted as
  `executor.yaml` by `artheia executor emit`.
- **Children-by-name.** Each SupervisorNode lists children as strings;
  resolution happens in `build_supervisor_tree`, where the name
  matches either another SupervisorNode or a `Process` from
  `rig.execution_manifests`.
- **The four restart strategies.** `one_for_one`, `one_for_all`,
  `rest_for_one`, plus `simple_one_for_one` (for dynamic children,
  unused today). Erlang-spec parity.
- **The reference hierarchy** (target shape — see "Translation" below):
  ```
  root
  └─ ar_sup
     ├─ core_sup        # exec, core, crypto, sm + network_sup + host_svc_sup + pltf_sup
     └─ app_sup         # app_1 .. app_N
  ```

### Changes

| Field | Today | Proposed | Reason |
|---|---|---|---|
| Per-child `restart_type` | absent (implicit `permanent`) | `permanent` / `transient` / `temporary` | The Erlang-spec annotates each leaf with one of these. Today the supervisor binary always restarts every leaf forever; we need `transient` for hardware-adapter leaves that may legitimately stop without escalation, and `temporary` for one-shot demo binaries. |
| Per-child `shutdown_timeout` | absent (hardcoded `5000` ms) | per-leaf integer (`shutdown: timeout_flush_logs` style → ms in our DSL) | `log` needs longer drain than `phm`. Today the supervisor SIGKILLs after a flat 5 s; the Erlang-spec calls for per-leaf tuning. |
| Per-supervisor `machine: Optional[str]` | absent | optional pin | A SupervisorNode without `machine` is workspace-wide; one with `machine="compute_host"` only ships in that machine's slice. **This is what enables the per-machine `execution.yaml` projection** (the core deliverable of this whole task). |
| Per-child overrides on a parent | absent | maybe via `ChildOverride` | Today all leaf options live on the leaf's `Process` entry. Erlang-spec puts restart/shutdown next to where the child is *listed*. Decision deferred to the impl turn — both shapes work. |

### Virtual vs real (the key constraint)

Erlang's `core_sup` is a real BEAM process (a `gen_server`). **Ours
is virtual** — the singular supervisor binary running on the
machine is the only real process; `core_sup`, `network_sup`, etc.
exist purely as **grouping + strategy declarations** that the host
supervisor applies when classifying restart cascades for its
children.

Consequence:

- Leaves resolve to real OS processes (`rig.execution_manifests`).
- Sub-supervisors (`core_sup`, `network_sup`, etc.) **never resolve
  to a Process**. They only resolve to other SupervisorNodes.
- A SupervisorNode with a `start_cmd` would be a category error and
  the build should reject it.

The current `build_supervisor_tree` already enforces this via name
resolution (a child either matches another SupervisorNode OR a
Process — never both). The design preserves this; nothing changes.

## Translation: Erlang-spec → Python DSL

### From the Erlang spec (`Erlang-style-supervisor-spec.md`)

```erlang
sm_daemon = #{
    start => {sm_daemon, start_link, [SmConfig]},
    restart => permanent,
    shutdown => timeout_medium
}
```

### Today's Python DSL (in `services/manifest/service.py`)

```python
SupervisorNode(
    name="core_sup",
    strategy=RestartStrategy.REST_FOR_ONE,
    children=["exec", "core", "crypto", "sm", "network_sup", "host_svc_sup", "pltf_sup"],
)
# 'sm' resolves to the Process named 'sm' in PROCESSES, whose
# start_cmd / shutdown_timeout / restart_type comes from its
# StartupConfig.
```

### Proposed Python DSL (after extension)

```python
SupervisorNode(
    name="core_sup",
    strategy=RestartStrategy.REST_FOR_ONE,
    children=["exec", "core", "crypto", "sm", "network_sup", "host_svc_sup", "pltf_sup"],
    machine=None,    # workspace-wide (this sub-tree exists on every
                     # machine that has at least one of its leaves)
)

# Leaf-level annotations stay on the Process side, where they already
# live. Re-statement on the SupervisorNode level would be redundant.
#
# What we DO add: a per-child restart-type override when the rig wants
# to depart from the Process's default. Today this is "wishful" — the
# supervisor binary's executor.yaml emits the Process's default
# regardless of where the leaf is mentioned. Strict-spec parity will
# require either:
#   (a) extending Process with restart-type / shutdown-timeout fields
#       (the spec-aligned move; AUTOSAR puts these in
#       StartupConfig already), or
#   (b) adding a `ChildOverride(name, restart_type, shutdown_ms)`
#       Sometime-typed entry to SupervisorNode.children.
#
# Recommendation: (a). The execution manifest already has
# StartupConfig.termination_behavior and friends; extend those.
```

### Per-machine pinning (the new field)

`SupervisorNode.machine: Optional[str]` enables:

```python
SupervisorNode(
    name="pltf_sup",
    machine="compute_host",   # only ships in compute's executor.yaml
    children=["shwa"],
)
```

When `build_supervisor_tree(rig, machine=...)` is called with a
machine pin, the result is the sub-tree where every leaf's
`Process.host_machine` matches **and** every SupervisorNode's
`machine` matches (or is `None` = workspace-wide).

Today's `build_supervisor_tree(rig)` (no machine arg) keeps working
as the whole-tree builder; per-machine emission goes through the
new arg.

`artheia executor emit` will then accept `--machine <name>` and emit
only that machine's slice — wiring up to `dist_manifest._execution_payload`
which already emits per-machine `execution.yaml`.

## Today vs target: concrete deltas

Walked from `today-executor.yaml` against the user-proposed shape:

| Leaf | Today's location | Target | Delta |
|---|---|---|---|
| `log` | **absent** (no daemon, syslogs only) | `host_svc_sup` | Add when log daemon lands |
| `camera` | absent (not in CLUSTERS) | `pltf_sup` | Future; placeholder when camera FC arrives |
| `per`, `rds` | `host_svc_sup` ✓ | same ✓ | no change |
| `phm`, `ucm`, `vucm` | `pltf_sup` ✓ | same ✓ | no change |
| `shwa` | `pltf_sup` (today, central) | should be on compute | **Move + add `machine='compute_host'` pin** |
| `fw` | `pltf_sup` (today, central) | not in target snippet | **Decide**: keep in pltf_sup? Or move to network_sup (it IS a firewall)? |
| `nm`, `com`, `osi`, `idsm`, `diag`, `tsync` | `network_sup` ✓ | same ✓ | no change |
| `exec`, `core`, `crypto`, `sm` | `core_sup` direct ✓ | same ✓ | no change |
| `demo_p1/p2/p3` | `app_sup` ✓ | `app_sup` (as `app_1`, `app_2`) ✓ | name choice — `demo_p1` is fine, the spec's `app_1` was placeholder |

Also: today's strategy choices already match the spec (one_for_all
root, rest_for_one ar_sup, rest_for_one core_sup, one_for_one for
the per-domain subs, one_for_one for app_sup). So **the strategies
are correct; only leaf placement + machine pinning need work**.

## Validation target

Once the DSL extension lands, building the demo rig should produce:

- `dist/manifest/central_host/execution.yaml` — the slice for central
  (every leaf except `shwa`, no `app_sup` since demos run on compute).
- `dist/manifest/compute_host/execution.yaml` — `shwa` under
  `pltf_sup`, plus `app_sup` with `demo_p1/p2/p3`.
- `dist/manifest/admin_host/execution.yaml` — empty (no supervisor on
  the operator workstation).

Acceptance: supervisor-gui's Applications panel (the OTP-style
drawn tree) renders both `central_host` and `compute_host` trees
side-by-side, each pulled from its own running supervisor over
`services/com`.

## Out of scope for this doc

- The runtime-side `Supervisor` class for in-binary actor supervision
  (item 2 of `README.md`). That's the C++ work. This DSL change
  precedes it but doesn't block it.
- gen-app-composition's emit changes (item 3 of `README.md`). Same
  story — separate turn.
- The tracer hook on supervisor-detected restarts (item 4). Smallest
  change of the four; lands last.

## Next concrete steps after this doc lands

1. Extend `SupervisorNode` with `machine: Optional[str]` field
   (artheia/manifest/supervisor.py).
2. Extend `build_supervisor_tree` with optional `machine` arg
   producing a sliced tree.
3. Extend `artheia executor emit` with `--machine <name>`.
4. Plumb into `dist_manifest._execution_payload`: emit
   `<machine>/execution.yaml` with the sliced tree.
5. Pin `shwa` (and any other compute-only leaves) in `service.py`'s
   SUPERVISORS list with `machine='compute_host'`.
6. Smoke test: central's tree no longer has shwa; compute's tree
   has shwa under pltf_sup + app_sup with demo_p1/p2/p3.




# `artheia gen-rig` — bootstrap a rig.py from system.art

[tag:blocked-by:system-art-aggregation]
[tag:blocked-by:artheia-dsl-recovery]

> **Blocked on DSL recovery.** The user's clarified direction is to NOT
> generate more of the current adhoc `Layer` shape (parallel `add_*` /
> `remove_*` / `override_*` lists). See
> `docs/tasks/TODO/artheia-dsl-recovery.md` for the recovery spec —
> gen-rig needs the recovered DSL as its emission target. The rest of
> this doc describes what gen-rig should do once both blockers clear.

## Why

Bootstrapping a new rig today means copying `demo/manifest/rig.py` and
surgically adapting:

- `SwComponent` factory (one per process binary, derived from the .art
  `prototype` lines)
- `Executable` factory (one per `on process X` group)
- `Process` factory (with scheduling defaults)
- A `Layer` instance combining the three
- A final `Rig = merge_layers(PlatformBase, [...])`

Most of that work is mechanical — pure function of the composition
name + the .art it lives in. Only the deployment-specific parts
(machine assignment, IP/port, CPU affinity, scheduling priority,
vehicle identity, supervisor tree) need human judgment.

Goal: a one-shot CLI that emits a *working* rig.py skeleton with all
the mechanical parts filled in and the human-judgment parts marked as
`TODO` comments. The user edits the skeleton; subsequent regens are
out of scope — this is bootstrap, not round-trip.

## Blocker

[`docs/tasks/TODO/system-art-aggregation.md`]: cross-file textX
imports. The generator walks `composition Platform { composition Services svc; composition Demo3Way demo }` to enumerate every
prototype. With today's forward-decl stubs (empty composition bodies),
the walker sees zero prototypes — the helper relies on real
`CompositionDecl` cross-references, not stubs.

Step 1 below (services rig regenerator) is technically unblocked
because each FC's `package.art` is self-contained; but it's also
mostly redundant with `services/manifest/fc.py`'s existing factories,
so the higher-leverage work is the platform/application generator.

## CLI shape

```
artheia gen-rig <composition-fqn> [OPTIONS]

Positional:
  <composition-fqn>            Dotted FQN of the top-level composition,
                               e.g. `system.demo.Demo3Way` or
                               `system.Platform`.

Required options:
  --out PATH                   Where to write the rig.py. Refuses to
                               overwrite an existing non-empty file
                               unless --force is passed.

Conventions / defaults (optional flags):
  --vehicle-name=<str>         VehicleIdentity.name (default: derive
                               from <out> dir's name, e.g. demo/manifest/
                               → "demo").
  --machine-name=<str>         Default machine to land processes on
                               (default: "<vehicle>_host").
  --bazel-package=<str>        Bazel package prefix for SwComponent
                               targets (default: derive from the
                               composition's package — e.g. `system.demo`
                               → `//demo`).
  --grpc-port=<int>            Default services/com gRPC port (default: 7700).
  --force                      Overwrite existing rig.py.
```

## Input → output mapping

Given a top-level composition like:

```
// platform/system/demo/component.art
package system.demo
composition Demo3Way {
    prototype CounterNode     counter_p1      on process P1
    prototype DriverNode      driver_p1       on process P1
    prototype TickerNode      ticker_p1       on process P1
    prototype ObserverNode    observer_p2     on process P2
    prototype IncrementerNode incrementer_p3  on process P3
    connect driver_p1.inc_out         to counter_p1.inc_in
    ...
}
```

The generator emits a `rig.py` containing:

### Derivable

- **SwComponent per process** (one per distinct `on process X` value):

  - `name`            ← process name (`demo_p1` ← `P1`, lower-cased
    with `<vehicle>_` prefix)
  - `bazel_target`    ← `<--bazel-package>/<name>_main` convention
  - `owner`           ← "platform" (constant — covers platform-FC and
    per-rig apps alike; can be overridden after)
  - `art_node`        ← `<composition-pkg>/<composition-name>` (the
    composition the binary materializes)

- **Executable per process**:

  - `name`              ← matches SwComponent name
  - `category`          ← `"APPLICATION_LEVEL"` (constant; user
    edits to `PLATFORM_LEVEL` if needed)
  - `build_type`        ← `BUILD_TYPE_RELEASE` (constant)
  - `reporting_behavior` ← `REPORTING_BEHAVIOR_INDIVIDUAL` (constant)
  - `root_sw_component_prototype` ← a name derived from
    the composition (`{process}_root`, application_type=node-class
    derived from the composition).

- **Process per executable**:

  - `name`                          ← matches Executable name
  - `executable`                    ← matches Executable name
  - `function_cluster_affiliation`  ← `""` for non-FC (user fills if
    this rig is a platform layer)
  - `state_dependent_startup_config[0].startup_config`:
    - `scheduling_policy`   ← derived from any `NodeToCPUMapping`
      targeting this process — else
      `SCHED_OTHER`.
    - `scheduling_priority` ← same source — else `0`.
    - `termination_behavior` ← `PROCESS_IS_NOT_SELF_TERMINATING`
      (constant; overridable).

- **Layer + Rig assembly**:

  - `set_vehicle=VehicleIdentity(name=<vehicle-name>, make="theia", model="<composition-fqn>")`
  - `add_machines=[<vehicle>Host]`
  - `add_components=[ ... ]`
  - `add_executions=[ ... ]`
  - `Rig = merge_layers(PlatformBase, [<Layer>])`

### Not derivable — emitted as TODO scaffolding

```python
# TODO(rig): set CPU arch, IP, gRPC endpoint, hardware resources.
<vehicle>Host = MachineManifest(
    name="<vehicle>_host",
    hardware=HardwareResource(
        cpu=CpuResource(architecture=CpuArchitecture.X86_64),  # TODO
    ),
    com_endpoint=IpEndpoint(
        address=IPv4Address("127.0.0.1"),                       # TODO
        port=7700,
    ),
)

# TODO(rig): supervisor tree — add SupervisorNodes if this rig needs
# a non-default tree. Default is to inherit PlatformBase's supervisor
# tree and add the application processes under app_sup.

# TODO(rig): per-process affinity / nice / scheduling priority — add
# NodeToCPUMapping entries to the Layer.
```

## Demo regeneration acceptance test

Running:

```
artheia gen-rig system.demo.Demo3Way \
  --out demo/manifest/rig.generated.py \
  --vehicle-name=demo \
  --machine-name=demo_host
```

Should produce a file equivalent to today's `demo/manifest/rig.py`
modulo:

- Cosmetic differences (comment style, blank lines, import order).
- `DemoHost.com_endpoint` being a TODO placeholder rather than the
  current `127.0.0.1:7700` literal.
- Per-process scheduling staying at defaults (today's file is also
  default — no divergence expected).
- Vehicle make/model strings (the current file says
  `"gen_server-demo"` for model — generated version will say
  `"system.demo.Demo3Way"`; functionally equivalent).

If `artheia executor emit` on the generated rig produces an
`executor.yaml` byte-identical to the current one, the generator is
correct.

## Implementation plan (sequenced)

### Phase 1 — services rig regenerator (UNBLOCKED today)

Less leverage but easy to ship: replace the hand-coded body of
`services/manifest/fc.py` (`_component_for` / `_executable_for` /
`_process_for`) with a templated factory that takes a list of
`(short, daemon_class, tipc_addr)` triples. The triples come from
`clusters.CLUSTERS` + the per-FC `package.art` (which the existing
`load_platform_services` already parses). Net: no behavior change,
the file becomes regeneration-source rather than hand-rolled.

Output: same `COMPONENTS`, `EXECUTABLES`, `PROCESSES`, `SUPERVISORS`
exports — but synthesised, not literal.

### Phase 2 — `gen-rig` CLI (BLOCKED on cross-file imports)

Adds `artheia gen-rig <fqn>` per the CLI shape above. Walks the
composition via `flatten_composition()` (already in
`artheia/model/flatten.py`), groups prototypes by their
`on process X` annotation, emits the SwComponent / Executable /
Process / Layer / Rig boilerplate to the output path.

Uses Jinja2 (already a deps via `cpp_app` templates) for the
output file — one `rig.py.j2` template under
`artheia/artheia/generators/templates/rig/`.

### Phase 3 — refine

- Sniff `NodeToCPUMapping` decls in the composition's package and
  emit corresponding affinity/scheduling overrides instead of bare
  defaults.
- Sniff `gateway_route` / `bus` decls and emit reasonable defaults
  for the machine's network adapters.

## Implementation notes / risks

- **Whose responsibility is `<vehicle>Host.com_endpoint`?** The
  current `services/manifest/fc.py` doesn't set it. The user fills it
  in `demo/manifest/rig.py`. The generator should default to a
  loopback endpoint with a TODO comment — multi-machine rigs need to
  edit it anyway.
- **PlatformBase vs custom platform layer**: the generated rig assumes
  the user wants `merge_layers(PlatformBase, [generated_layer])`. If
  the user is generating a NEW platform layer (i.e. replacing
  PlatformBase entirely), the CLI needs a `--no-platform-base` flag.
  Defer to phase 3.
- **Round-trip regen is OUT OF SCOPE**: this is bootstrap. Once the
  user edits the generated rig.py, regenerating would clobber their
  edits. If round-trip becomes a goal later, we'd need a comment-marker
  scheme (`# BEGIN/END auto-generated`) — but the user pushed back on
  that idea in the discussion that produced this task.
- **Forward-decl stubs in component.art**: today's stub nodes inside
  `component.art` (with bare `tipc type=...`) provide enough
  information for the generator IF the walker reads them from the
  same file. But if the composition lives in `component.art` and
  references nodes from `package.art`, the cross-file resolution
  is needed even for single-component compositions. Confirm
  during phase 2.

## Out of scope

- Round-trip regen (clobbering user edits).
- A generator for `services.manifest.fc.SUPERVISORS` — the supervisor
  tree shape is an operational decision (rest_for_one ordering,
  per-domain grouping) that can't be derived from .art.
- Multi-machine deployment rigs — the generated machine is always
  a single host; multi-machine support means user-edits.

# Resolution (2026-05-22)

`artheia gen-rig <art_file> --composition <name> --out <path>` lands.

Generator (`artheia/generators/rig.py`):
- Extracts the composition from the .art file via
  `flatten_composition` (which handles nested `composition Foo bar`
  refs once cross-file imports clear — today flat compositions work).
- Groups prototypes by `on process X` annotation, deterministic.
- Emits a runnable Python module that exports `<Vehicle>Software:
  SoftwareSpecification = FcSoftware.squash(<Vehicle>SpecLayer)` —
  the structured-DSL shape, NOT the legacy `Layer`/`merge_layers`
  path (per the DSL-recovery direction).

CLI (`artheia gen-rig`):
- `--composition <name>` selects the top-level composition.
- `--out <path>` writes the rig.py (refuses non-empty existing).
- `--vehicle-name`, `--machine-name`, `--bazel-package` overridable;
  sensible defaults derive from `--out`.
- `--grpc-port`, `--force` for the obvious.

Acceptance: regenerating today's `demo/manifest/rig.py` from
`Demo3Way` and running `artheia executor emit` produces byte-
identical `executor.yaml`. 5 new tests
(`tests/test_gen_rig.py`) cover the path.

## Remaining work

Both blockers cleared for the demo case (single-file composition):
- `artheia-dsl-recovery.md` — done.
- `system-art-aggregation.md` — only matters when generating from a
  composition-of-compositions (`composition Platform { composition
  Services svc; composition Demo3Way demo }`). Single-file
  compositions like `Demo3Way` work today.

When cross-file imports land, gen-rig will work on the top-level
`Platform` composition too — no changes to the generator needed;
just `flatten_composition` will see real prototypes inside the
inner refs.

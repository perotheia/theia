# Recover the manifest DSL — `Layer.squash` + embedded set transforms

## Why this matters

The current `artheia/manifest/Layer` is *adhoc*: seven parallel list
families (`add_machines`, `remove_machines`, `override_machines`,
`add_components`, `remove_components`, `override_components`, …) one
per element kind. It works, but:

- Adding a new manifest element kind (e.g. for the AUTOSAR
  Application Manifest with its own SwComponent + composition
  bag, or for ExecutionManifest scheduling overrides) means adding
  three more parallel lists — combinatorial growth.
- The DSL doesn't reflect the actual containment shape. A
  ServiceManifest contains ServiceInstances; a MachineManifest
  contains hardware resources; an Application contains components.
  Today these are all expressed at one flat level, with the user
  reading identity fields to figure out what overrides what.
- Composition is a free function (`merge_layers(base, [l1, l2, …])`)
  not a method (`base.squash(l1).squash(l2)`). Less chainable, no
  natural place for type narrowing.
- There's no representation of "unset / use the base's value"
  (`Undefined`), so vendor layers have to restate everything.

We had the right shape once — `theia_runtime/artheia/artheia/manifest/transform.py`
(372 lines) implements it, ported from Mosaic's
`tools/syscomp/engine/transform.py`. The current `theia/artheia/artheia/manifest/transform.py`
(193 lines) is a simplification that dropped the key DSL features in
the rush to land the Adaptive-AUTOSAR rename (tasks #177-180 in our
task list).

The user's bar:

  > If we cant recover DSL style i honestly dont want this code.

This task recovers it.

## Target DSL — what we want to write

Modeled on the legacy mosaic raj_syscomp.py
(`/home/axadmin/up/orchestate/vehicles/.../raj_syscomp.py`):

```python
from artheia.manifest.transform import Append, Remove, SetTransformTypes
from artheia.manifest.model import (
    Application, MachineManifest, SoftwareSpecification,
    ServiceInstance, SwComponent, Process,
)
from typing import cast


# The base layer for the Macan vehicle (a hand-authored spec).
MacanSoftware: SoftwareSpecification = ...  # imported from vehicles/macan/

# A rig-specific layer that REFINES MacanSoftware.
RajLayer = SoftwareSpecification(
    machines=cast(
        set[SetTransformTypes],
        {
            Append(
                MachineManifest(
                    name="central_compute",
                    applications=cast(
                        set[SetTransformTypes],
                        {
                            Remove(SwComponent(name="tell_tale_controller")),
                            Append(SwComponent(name="climate_arbitrator", ...)),
                            Append(SwComponent(name="signal_probe_server", ...)),
                        },
                    ),
                    com_endpoint=IpEndpoint(IPv4Address("127.0.0.1"), 7700),
                ),
            ),
            Remove(MachineManifest(name="adas_compute")),
        },
    ),
    vehicle=VehicleIdentity(name="raj", make="Porsche", model="Macan"),
)

# Compose. Each call returns a new (immutable) spec.
RajSoftware = MacanSoftware.squash(RajLayer)
```

Three things this gives us that today's shape doesn't:

1. **Containment matches reality** — `applications` is a property of a
   `MachineManifest`, not a sibling list at top level. The user
   writes refinements INSIDE the parent they target.
2. **Transforms ARE the elements** — an `Append(X)` sitting in the
   `machines` set means "add or merge X". A `Remove(X)` means
   "drop the machine with this identity". The set IS the
   transformation; no need for parallel `add_/remove_/override_`
   triplets.
3. **`.squash()` chains** — `Base.squash(L1).squash(L2)` reads
   left-to-right (base first, refinements applied in order). The
   method lives on the spec type, so IDEs can suggest fields and
   typecheckers can catch shape mismatches.

## What's currently missing — gap analysis

`theia/artheia/artheia/manifest/transform.py` (193 lines) is missing,
relative to `theia_runtime/.../transform.py` (372 lines):

| Feature | Today | Target |
|---|---|---|
| `Layer` base class with `.squash(other)` method | absent | `Layer[T]`, recurses through `__dataclass_fields__` |
| Recursive `squash` for nested `Layer`s | absent | a Layer field gets `var.squash(other_var)` |
| Set transforms `Append(x)` / `Remove(x)` with `.apply(current)` | inert dataclasses | callable transforms |
| `transform_set` / `set_squash` | absent | composes a set-of-transforms onto a base set |
| `Identifiable._set_identify` (set-membership identity) | only list-position via `_identity` | yes — sets are unordered, identity is what matters |
| `Undefined` / `Default` value markers | absent | per-field "use base value" sentinels |
| `Defer(callable)` for late-bound values | absent | resolved during `simplify()` |
| `simplify()` to materialize a `Layer[T]` into a concrete `T` | absent (Layer == Rig) | explicit conversion step |
| `SoftwareSpecification` as the top-level Layer | n/a — we have `Layer` (with parallel lists) and `Rig` (a flat aggregator) | one type that's both the spec AND the layer; `squash` collapses |

## Migration plan — sequenced, minimal disruption

### Phase 0 — port the transform engine

1. Copy `theia_runtime/artheia/artheia/manifest/transform.py` →
   `theia/artheia/artheia/manifest/transform.py`. (Side-by-side
   diff exists; merge wins are obvious.) Keep the existing
   `Add`/`Remove`/`Override`/`Op`/`apply_ops` API momentarily as
   thin shims that wrap the new `Append`/`Remove`/`squash` semantics,
   so dependents keep compiling.

2. Add `Layer[T]`, `Identifiable[T]`, `Append`, `Remove`,
   `SetTransform`, `transform_base`, `transform_set`, `set_squash`,
   `Undefined`, `Default`, `Defer`, `merge_field`, `simplify`.

3. Unit tests in `artheia/tests/test_transform.py` covering:
   - Squashing two layers of the same type
   - `Append` adds new + merges existing-by-identity
   - `Remove` drops by identity
   - Nested set transforms (Append inside a layer inside a set
     inside another layer)
   - `Undefined` / `Default` fall-back semantics
   - `Identifiable._set_identify` — non-name identity fields

### Phase 1 — restructure manifest dataclasses

The dataclasses in `artheia/manifest/{application,execution,machine,
rig,service,supervisor}.py` need to:

1. Inherit from `Layer[<their concrete resolved type>]` (or just be
   `Layer` subclasses if there's no separate resolved type).
2. Mark each field that's a set-of-X as `set[X] | set[SetTransformTypes]
   | Undefined[set[X]]`. Today these are `list[X]` with
   `default_factory=list`.
3. Mark each scalar field that should support `Undefined` (i.e.
   inheritable from base layer) as `T | Undefined[T]`. Defaults
   become `Undefined()`.

Concrete file-by-file impact (estimated):

- `application.py` — `ApplicationManifest` gains `Layer` base; `components`
  becomes `set[SwComponent] | set[SetTransformTypes]`. Same for
  `Executable`, `Process`.
- `execution.py` — `ExecutionManifest` becomes a Layer.
- `machine.py` — `MachineManifest` becomes a Layer; `applications`
  field added (currently apps are tracked separately on `Rig`).
- `service.py` — `ServiceManifest` becomes a Layer; `instances` set.
- `rig.py` / `layer.py` — these collapse into a single
  `SoftwareSpecification` type. The 7 parallel `add_*` / `remove_*`
  / `override_*` families are replaced by single set-typed fields
  that accept either bare elements OR `Append/Remove` wrappers.
- `supervisor.py` — `SupervisorNode` becomes a Layer with
  `children: set[str] | set[SetTransformTypes]`.

### Phase 2 — migrate call sites

`services/manifest/fc.py` and `demo/manifest/rig.py` need rewrites
into the new shape. After:

```python
# services/manifest/fc.py — the base platform spec
PlatformSoftware = SoftwareSpecification(
    machines=set(),  # left for vehicle layers
    services=ServiceManifest(
        name="platform_services",
        instances=cast(set[SetTransformTypes], {
            Append(ServiceInstance(name=fc.short, ...))
            for fc in CLUSTERS
        }),
    ),
    applications={
        # ApplicationManifest with one SwComponent per FC.
        ApplicationManifest(
            name="platform_app",
            host_machine=Undefined(),  # vehicle layer fills this
            components=cast(set[SetTransformTypes], {
                Append(_component_for(fc.short)) for fc in CLUSTERS
            }),
        ),
    },
    supervisor_tree=...,  # the OTP supervisor tree (unchanged)
)

# demo/manifest/rig.py — refines the platform layer
DemoLayer = SoftwareSpecification(
    vehicle=VehicleIdentity(name="demo", make="theia", model="gen_server-demo"),
    machines=cast(set[SetTransformTypes], {
        Append(MachineManifest(
            name="demo_host",
            hardware=HardwareResource(cpu=CpuResource(architecture=CpuArchitecture.X86_64)),
            com_endpoint=IpEndpoint(IPv4Address("127.0.0.1"), 7700),
            applications=cast(set[SetTransformTypes], {
                # The demo binaries land on this host.
                Append(SwComponent(name="demo_p1", bazel_target="//demo:p1_main", ...)),
                Append(SwComponent(name="demo_p2", bazel_target="//demo:p2_main", ...)),
                Append(SwComponent(name="demo_p3", bazel_target="//demo:p3_main", ...)),
            }),
        )),
    }),
)

DemoRig = PlatformSoftware.squash(DemoLayer)
```

Note how:
- The demo's three binaries land INSIDE `demo_host.applications` —
  containment is explicit.
- The fact that `demo_host` is added (`Append(MachineManifest(...))`)
  alongside the existing `Append`-inside-`ApplicationManifest` for
  `host_machine="demo_host"` is no longer needed — the binding is
  structural.
- Removing a demo process is `Remove(SwComponent(name="demo_p2"))`
  inside `demo_host.applications`, not `remove_components=["demo_p2"]`
  at top level.

### Phase 3 — migrate downstream

- `artheia.cli.generate_manifest` — change the YAML emitter to walk
  the new `SoftwareSpecification` shape (recursively, since
  containment now matches the YAML structure better).
- `artheia.cli.executor emit` — same.
- `artheia.cli.gui emit` — same.
- `build_supervisor_tree` in `manifest/supervisor.py` — walks the
  new structure rather than `Rig.applications + Rig.process_to_machine_mappings`.
- The legacy `Add`/`Override`/`apply_ops` shims from phase 0 can be
  deleted once all call sites move.

### Phase 4 — `gen-rig` (now unblocked from the DSL angle)

With the recovered DSL, `artheia gen-rig <fqn> --manifest <kind>`
emits source in the new shape. Per the user's CLI direction:

```
artheia gen-rig <fqn> \
    --manifest [Service|Application|Machine|Execution] \
    --out path/to/file.py
```

- `--manifest Service`     → emit `services/manifest/services.py` with
                             `PlatformServices: ServiceManifest = ...`.
- `--manifest Application` → emit `demo/manifest/application.py` with
                             `DemoApplication: ApplicationManifest = ...`.
- `--manifest Machine`     → emit `demo/manifest/rig.py` (or append to
                             it) with the machine block.
- `--manifest Execution`   → emit into the same `rig.py`, the
                             per-process scheduling block.

The generator's job stays focused on the structural / mechanical
parts (one SwComponent per `prototype`, one Executable per
`on process X` group, default scheduling), with `Undefined()` /
`TODO`-marked fields for everything that requires human judgment
(machine endpoint, CPU affinity, vehicle identity, supervisor
tree shape).

See `docs/tasks/BACKLOG/generate-rig-from-system.md` for the gen-rig
detail — that ticket comes back off blocked status once phase 2 of
this task lands.

## Risks + non-trivialities

- **Existing test suite**: artheia/tests/ has 60 tests, most of which
  parse .art files; only a few test the manifest model. The grammar
  side is unaffected. The few manifest tests will need updates;
  they're integration-level (executor emit produces expected YAML),
  so a recipe of "regenerate snapshot YAML, diff against the old
  one" works.

- **Order of set iteration**: Python `set` iteration order is not
  guaranteed. The legacy mosaic engine relied on this — but
  generated YAML / executor.yaml output IS order-sensitive (the
  supervisor's children-list order matters: `core` must come up
  before `crypto` in a `rest_for_one` strategy). Mitigation: emit
  to a list, sort by a stable key (identity / declaration order
  hint) before writing. Need an explicit "declaration order"
  annotation on `Identifiable` or a deterministic sort key.

- **`Defer`** is the value type for "this field needs late binding
  to context X" — used by the legacy mosaic to defer a service-instance
  address until the machine binding is known. We don't use it today
  but the gateway / bus assignments may want it. Port the type but
  don't make it load-bearing in phase 1.

- **typing.cast(set[SetTransformTypes], {...})** is the legacy idiom
  for set-of-transforms vs set-of-elements. It's ugly but type-checker-
  necessary. Phase 1 can introduce a `transforms({Append(...), Remove(...)})`
  helper that does the cast and reads better. Not load-bearing.

- **`Undefined`-as-default**: today every field has a real default
  (empty list, empty string, `SchedulingPolicy.SCHED_OTHER`, etc.).
  Switching defaults to `Undefined()` is a Big Change because every
  reader of the data has to handle the `Undefined` case. Mitigation:
  do this ONLY for fields that vehicle layers actually want to
  inherit. Most scalars can keep their concrete defaults.

## Acceptance criteria

- `artheia executor emit demo.manifest.rig --rig DemoRig` produces
  the exact same `executor.yaml` it does today (byte-identical
  except for stable sort).
- `artheia gui emit demo.manifest.rig --rig DemoRig` produces the
  same `machines.yaml`.
- `services/manifest/fc.py` and `demo/manifest/rig.py` read
  noticeably more like the legacy mosaic raj_syscomp.py — every
  refinement lives INSIDE the parent it targets.
- New TODO `gen-rig` task is unblocked (becomes "consumer of this
  DSL").
- 60 artheia tests pass; ideally 65+ with the new transform tests.

## What "done" looks like for THIS spec ticket

This file gets read and either:
- Acked, and we proceed to phase 0 in a follow-up.
- Rejected with specific changes — possibly because something in
  the legacy mosaic shape doesn't translate (e.g. our composition
  layer in .art is single-file today; cross-file imports are also
  blocked; maybe the right answer is to do those FIRST and then
  recover the DSL).
- Reframed — maybe the gap isn't worth closing in one big sweep
  and we want a series of smaller incremental rebuilds.

The user said "Please think hard how to do it before implementing
generation. If we cant recover DSL style i honestly dont want this
code." This spec is the deliverable for the "think hard" step.

## Related tasks

- `docs/tasks/TODO/system-art-aggregation.md` — independent. Cross-file
  textX imports. Needed for gen-rig phase 2 (walking `composition
  Platform { composition Services svc }`), unrelated to this DSL
  work.
- `docs/tasks/BACKLOG/generate-rig-from-system.md` — depends on
  this. Updated to point here.
- `docs/tasks/TODO/autosar-regen-package-names.md` — independent.
  Autosar regenerator updates.


# Resolution (2026-05-22)

All 5 phases landed.

- **Phase 0** — ported `transform.py` from theia_runtime. Added
  `Layer.squash`, `Append`/`Remove` set transforms, `Undefined`/`Default`/
  `Defer` value markers, `simplify`. Kept legacy `Add`/`Override`/
  `apply_ops` as compat shims at the bottom.
- **Phase 1** — added `SoftwareSpecification(Layer)` to
  `manifest/rig.py`. Set-typed fields default to `Undefined()` so a
  layer that omits a field inherits the base's value during squash.
  Added `to_rig()` bridge for the legacy CLI.
- **Phase 2** — added `identifiable_dataclass` decorator that
  restores `__hash__` + `__eq__` after `@dataclass` runs. Switched
  37 manifest `Identifiable` subclasses from `@dataclass` to
  `@identifiable_dataclass`. Now hashable for set membership.
- **Phase 3** — adopted in `services/manifest/fc.py` and
  `demo/manifest/rig.py`. Both files now export both the legacy
  (`FcLayer`/`DemoRig`) and the new (`FcSoftware`/`DemoSoftware`)
  shapes. Equivalent — verified by test
  `test_demo_software_to_rig_equivalent_to_legacy_demo_rig`.
- **Phase 4** — CLI (`artheia executor emit`, `artheia gui emit`,
  `artheia generate-manifest`) gains a `_resolve_rig` helper that
  accepts either `Rig` or `SoftwareSpecification` exports and
  auto-materializes via `to_rig()`. Auto-pick prefers
  `*Software` over `*Rig` for new code. Added `list[Identifiable]`
  branch to `Layer.squash` so same-identity `ApplicationManifest`
  squashes merge their `components` lists (FC + rig binaries)
  instead of wholesale replace.

`artheia executor emit demo.manifest.rig` produces byte-identical
`executor.yaml` whether invoked via `DemoRig` (legacy) or
`DemoSoftware` (new). 100 tests pass (was 60 pre-session).

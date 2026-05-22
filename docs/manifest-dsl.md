# Manifest DSL — `Layer.squash` + `SoftwareSpecification`

Reference for the structured manifest DSL that landed in
`docs/tasks/DONE/artheia-dsl-recovery.md`. Companion to the per-file
reference in `artheia/manifest/README.md`.

## TL;DR

A vendor rig is one Python expression:

```python
from artheia.manifest.rig import SoftwareSpecification
from artheia.manifest.transform import Append, Remove, SetTransformTypes
from services.manifest.fc import FcSoftware
from typing import cast

DemoSpecLayer = SoftwareSpecification(
    vehicle=VehicleIdentity(name="demo", make="theia", model="..."),
    machines=cast(set[SetTransformTypes], {
        Append(MachineManifest(name="demo_host", ...)),
    }),
    applications=cast(set[SetTransformTypes], {
        Append(ApplicationManifest(name="platform_app", host_machine="demo_host", ...)),
    }),
    execution_manifests=cast(set[SetTransformTypes], {
        Append(p) for p in DEMO_PROCESSES
    }),
)

DemoSoftware = FcSoftware.squash(DemoSpecLayer)
```

`artheia executor emit demo.manifest.rig --rig DemoSoftware` writes
the supervisor's `executor.yaml`. `artheia gui emit` writes the
GUI's `machines.yaml`. Both also accept legacy `Rig` exports during
the migration window — see `_resolve_rig` in `artheia/cli.py`.

## Concepts

### Layer

Any `@dataclass` that inherits from `artheia.manifest.transform.Layer`
gets a `.squash(other)` method. `squash` walks the dataclass fields
and merges from `other` according to field kind:

| field kind                 | merge rule                                     |
|----------------------------|------------------------------------------------|
| `set[...]`                 | apply `Append` / `Remove` transforms (see below) |
| nested `Layer`             | recurse: `var.squash(other_var)`               |
| `list[Identifiable]`       | union by identity (legacy `_merge_lists`)      |
| scalar (everything else)   | `other` wins if set; else keep `var`           |

`other_var` can be `Undefined()` to signal "I don't touch this
field" — keeps `var`'s value.

### Identifiable

`Identifiable(Layer)` is the mixin every set-element manifest type
inherits from. It provides:

- `_identity_field: str = "name"` — the dataclass field whose value
  identifies the element. Override only if your identity isn't named
  `name`.
- `_identity` — the value of that field.
- `_set_identify: int` — `hash(self._identity)` by default. Used by
  `Append` / `Remove` to find same-identity members inside a set.
- `__hash__` / `__eq__` — identity-based, so instances live in
  `set[X]` directly.

Subclasses must use the `@identifiable_dataclass` decorator instead
of bare `@dataclass`. The stdlib `@dataclass` (with default
`eq=True`) clobbers `__hash__` to None, breaking set membership;
`identifiable_dataclass` restores it after the dataclass decorator
runs.

```python
from artheia.manifest.transform import Identifiable, identifiable_dataclass

@identifiable_dataclass
class MyComponent(Identifiable):
    name: str
    target: str = ""
```

### Set transforms — `Append` / `Remove`

Set-typed fields on a `Layer` accept either bare elements or
transform wrappers:

```python
machines={Append(host_a), Append(host_b)}          # add two hosts
machines={Append(host_a), Remove(MachineManifest(name="bad"))}
```

`Append.apply(current)` semantics:
- New identity → adds the value to the set.
- Existing identity → squashes the new value into the existing one
  (field-merge via `Layer.squash`).

`Remove.apply(current)` semantics:
- Matches by `_set_identify`. Drops the matching element if present;
  silently no-ops otherwise.

The `cast(set[SetTransformTypes], {...})` boilerplate is the legacy
mosaic idiom — required by type checkers but visually noisy. A
helper `transforms({...})` would tidy it; not landed yet.

### `SetTransformTypes`

Alias for `Union[Append[Identifiable], Remove[Identifiable]]`.
Lives in `artheia/manifest/transform.py`. Useful as the type hint
for set fields that accept either bare elements or transforms.

### Value markers

| Marker            | Meaning                                             |
|-------------------|-----------------------------------------------------|
| `Undefined()`     | "Unset; inherit base layer's value." Default-factory for unset fields. |
| `Default(x)`      | "Use `x` if every layer above is also unset." Resolved at `simplify()` time. |
| `Defer(fn)`       | "Resolve lazily by calling `fn(context)`." Used for fields whose value depends on machine-binding done in a higher layer. |

A `Defer` survives `squash`; it must be resolved before
`simplify()` materializes the spec into a concrete value.

### `simplify()` vs `to_rig()`

Two ways to materialize a `Layer[T]` into a concrete spec:

- **`simplify(context=None)`** — generic. Walks the field tree,
  resolves `Default` to its inner value, raises if a stray
  `Undefined` or `Defer` survived. Returns the same dataclass type
  with all-concrete values.
- **`SoftwareSpecification.to_rig()`** — bridge to the legacy `Rig`
  dataclass. Converts each set-typed field to a deterministically-
  sorted list, fills `Undefined` with `[]`, returns a `Rig` for
  callers that haven't migrated yet (the CLI's `_resolve_rig`
  auto-applies this).

## File layout (after recovery)

| File | Role |
|---|---|
| `transform.py` | DSL engine: `Layer` + `Identifiable` + `Append`/`Remove` + `Undefined`/`Default`/`Defer` + `identifiable_dataclass` decorator. Legacy `Add`/`Override`/`apply_ops` compat shims at the bottom. |
| `rig.py` | `VehicleIdentity`, legacy `Rig`, new `SoftwareSpecification`. `SoftwareSpecification.to_rig()` bridges between the two. |
| `application.py`, `execution.py`, `machine.py`, `service.py`, `supervisor.py` | 37 `Identifiable` subclasses, all using `@identifiable_dataclass`. |
| `layer.py` | Legacy flat-list `Layer` (with `add_machines` / `remove_machines` / `override_machines` per element kind). Used by `services/manifest/fc.py::FcLayer` and `demo/manifest/rig.py::DemoLayer` until those call sites complete migration. |
| `platform.py` | `PlatformBase` (legacy Rig) and `PlatformServices` (ServiceManifest) — composed from the FC catalog. |
| `clusters.py` | Catalog of 18 Adaptive AUTOSAR Functional Clusters. |
| `loader.py` | textX-driven loader: per-FC `package.art` → `ServiceManifest` + `Process` list. |
| `supervisor.py` | `build_supervisor_tree(rig)` — materializes the OTP supervisor tree from a Rig. |

The legacy and new shapes coexist during the migration window. New
code uses `SoftwareSpecification` + `Append`/`Remove`. Old call
sites stay on `Rig` + `Layer` + `apply_ops` until they migrate.

## Composition pattern (mosaic style)

A platform supplier publishes a base spec; each rig publishes a
delta layer; the final spec is `base.squash(rig_layer).squash(...)`:

```python
# Platform layer (e.g. services/manifest/fc.py)
FcSoftware: SoftwareSpecification = SoftwareSpecification(
    applications={Append(PlatformApplication)},  # 18 FC components
    execution_manifests={Append(p) for p in PROCESSES},
    supervisors={Append(s) for s in SUPERVISORS},
)

# Vehicle layer (e.g. demo/manifest/rig.py)
DemoSpecLayer = SoftwareSpecification(
    vehicle=VehicleIdentity(name="demo", ...),
    machines={Append(DemoHost)},
    applications={
        # Same-identity Append — squash merges components.
        Append(ApplicationManifest(
            name="platform_app", host_machine="demo_host",
            components=DEMO_COMPONENTS,
        )),
    },
    execution_manifests={Append(p) for p in DEMO_PROCESSES},
)

# Final spec.
DemoSoftware = FcSoftware.squash(DemoSpecLayer)
```

When `DemoSoftware.applications` is materialized, the same-identity
`platform_app` from base and layer merge: `host_machine` switches
to `"demo_host"` (layer wins), `components` unions FC's 18 plus
demo's 3 (the `list[Identifiable]` branch in `Layer.squash` does
this — both lists carry `SwComponent`s, so they merge by identity).

## Field semantics on `SoftwareSpecification`

| Field                          | Type                                       | Default       |
|--------------------------------|--------------------------------------------|---------------|
| `vehicle`                      | `VehicleIdentity \| Undefined`             | `Undefined()` |
| `machines`                     | `set[MachineManifest] \| set[SetTransformTypes] \| Undefined` | `Undefined()` |
| `applications`                 | same shape                                  | `Undefined()` |
| `service_manifests`            | same shape                                  | `Undefined()` |
| `execution_manifests`          | same shape                                  | `Undefined()` |
| `process_to_machine_mappings`  | same shape                                  | `Undefined()` |
| `node_to_cpu_mappings`         | same shape                                  | `Undefined()` |
| `supervisors`                  | same shape                                  | `Undefined()` |

Set-typed fields default to `Undefined()` (not `set()`) so a layer
that omits a field inherits the base's value during squash.
Empty-set defaults would silently wipe the base — common footgun
the runtime version codified as a sentinel.

## CLI surface

```
artheia executor emit <module> [--rig <attr>]    → executor.yaml
artheia gui emit      <module> [--rig <attr>]    → machines.yaml
artheia generate-manifest <module> [--rig <attr>] → full rig YAML
artheia gen-rig <art_file> --composition <name> --out <path>
```

All three emit commands accept either a `Rig` or a
`SoftwareSpecification` attribute. Auto-pick (no `--rig` flag)
prefers `*Software` over `*Rig` over plain `Rig`.

See `docs/tutorials/gen-rig.md` for the bootstrap tutorial.

## Migration cookbook

### Adding a new manifest element type

1. Add the `@identifiable_dataclass`-decorated class in the right
   `manifest/*.py` file. Inherit `Identifiable` if it's an element
   that lives in a set; inherit only `Layer` (via Identifiable) if
   it's a top-level container.
2. Add the corresponding set-typed field to `SoftwareSpecification`
   with `Union[set[YourType], set[SetTransformTypes], Undefined]`
   and `default_factory=Undefined`.
3. Update `SoftwareSpecification.to_rig()` to project the set into
   a sorted list on the legacy `Rig` dataclass.
4. If the CLI/codegen consumes it, update the consumer to walk the
   new field.

### Writing a new vehicle layer

1. Import `SoftwareSpecification`, `Append`, `Remove`,
   `SetTransformTypes`.
2. Construct the layer as a `SoftwareSpecification` literal with
   inline `Append(...)` / `Remove(...)` in the set fields you touch.
   Leave untouched fields out (they default to `Undefined()`).
3. Final spec: `BaseSoftware.squash(YourLayer)`.

### Editing the demo (`demo/manifest/rig.py`) — example

To remove a process from the demo:

```python
DemoSpecLayer = SoftwareSpecification(
    ...
    execution_manifests=cast(set[SetTransformTypes], {
        Append(p) for p in DEMO_PROCESSES if p.name != "demo_p3"
        # Or, idempotently:
        # *(Append(p) for p in DEMO_PROCESSES),
        # Remove(Process(name="demo_p3")),
    }),
)
```

To override a process's scheduling policy:

```python
custom_p1 = Process(
    name="demo_p1",
    executable="demo_p1",
    state_dependent_startup_config=[
        StateDependentStartupConfig(...,
            startup_config=StartupConfig(
                scheduling_policy=SchedulingPolicy.SCHED_FIFO,
                scheduling_priority=10,
                ...
            ),
        ),
    ],
)

DemoSpecLayer = SoftwareSpecification(
    ...
    execution_manifests=cast(set[SetTransformTypes], {
        Append(custom_p1),     # same identity → squashes into base
        Append(p) for p in DEMO_PROCESSES if p.name != "demo_p1"
    }),
)
```

Same-identity Appends merge via `Layer.squash`, so `custom_p1`
overlays just the fields it differs in.

## Open work

- Legacy `apply_ops` / `Add` / `Override` compat shims can be
  retired once every call site stops using them.
- `simplify()` is implemented in `transform.py` but not yet wired
  into the CLI emit path (the CLI uses `to_rig()` instead).
  Future work might unify them.
- `Defer` is implemented but not yet load-bearing in any vehicle
  spec. The gateway / bus-affinity assignments are candidates.
- The `cast(set[SetTransformTypes], {...})` ceremony at every
  literal call site is a syntactic wart. A `transforms({...})`
  helper would tidy it.

## Related docs

- `artheia/manifest/README.md` — per-file reference.
- `docs/tutorials/gen-rig.md` — bootstrap a rig.py from .art.
- `docs/AUTOSAR/manifest.md`, `docs/AUTOSAR/adaptive.md` —
  upstream Adaptive AUTOSAR manifest spec.
- `docs/tasks/DONE/artheia-dsl-recovery.md` — the spec ticket.

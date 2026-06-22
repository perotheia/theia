# The `manifest` / rig.py composition algebra

How a deploy rig (`demo/manifest/<rig>/rig.py`) composes a `DeploymentLayer`.
Source of truth: `artheia/artheia/manifest/algebra.py` (the monoid engine) +
`artheia/artheia/manifest/deployment.py` (the four ARA axes). This is the
deploy-side counterpart to [art-lang-grammar.md](art-lang-grammar.md): the
`.art` describes the SYSTEM; a rig describes WHERE/HOW it deploys.

`theia manifest <rig>` imports `manifest.<rig>.rig`, reads its `RIG`
(a `DeploymentLayer`), `validate()`s it, then serializes per-machine JSON.

## The four orthogonal axes

A `DeploymentLayer` is the product of four independent axes — each answers one
question and composes on its own:

| axis (field)            | layer types                         | question |
| ----------------------- | ----------------------------------- | -------- |
| `execution`             | `ExecutionLayer{processes}` of `ProcessLayer` | WHAT processes run, on which machine, sched/affinity |
| `service`               | `ServiceLayer{instances}` of `ServiceInstanceLayer` | HOW they talk (interface, instance id, endpoint, provided_by) |
| `machines`              | `MachineSetLayer{machines}` of `MachineLayer` | WHERE — the ECUs (name, arch, cores, states) |
| `applications`          | `ApplicationSetLayer{applications}` of `ApplicationLayer` | the AA grouping (which processes, which host) |

`combine()` folds each axis independently — there is no cross-axis coupling at
combine time (cross-axis CONSISTENCY is checked later by `validate()`).

## ConfigField — scalar precedence (the field-level monoid)

Every scalar field on a `*Layer` is a `ConfigField`. When two layers combine,
each field combines by these rules:

| marker        | meaning                                   | combine behaviour |
| ------------- | ----------------------------------------- | ----------------- |
| `Explicit(v)` | a concrete value                          | wins over anything below; upper Explicit wins |
| `Default(v)`  | a fallback                                | loses to a concrete base; else carries |
| `Undefined()` | not set (the monoid identity)             | inherits whatever the other side has |
| `Defer(fn)`   | computed at simplify time                 | upper Defer wins |

A field left `Undefined` with no default that's still unset at `simplify()` is
a **required-field error**. Use `Explicit("central")` to pin, `Default(0)` for a
fallback.

```python
MachineLayer(name="central", arch=Explicit("aarch64"), cores={0, 1, 2, 3})
ProcessLayer(name="tsync", machine=Explicit("central"))   # bind to a machine
```

## Set fields — `Append` / `Remove` (the monoid set edits)

`processes`, `instances`, `machines`, `applications` are SET fields. A set field
holds **EITHER plain members OR a set of edits — never both**. To MODIFY a set a
lower layer contributed (e.g. `BASE`), the overlay carries edits:

- **`Append(X)`** — add `X`; or if `X` is `Identifiable` (every `*Layer` is) and
  a member with the same identity (`name`) exists, `combine()` them.
- **`Remove(X)`** — drop the member matching `X`. For an `Identifiable`, match is
  by the IDENTITY field only — write `Remove(ProcessLayer(name="p1"))`; the other
  fields are ignored. (Else by `==`.)
- Aliases: `Insert = Append`, `Delete = Remove`.
- In one edit set, **all `Remove`s apply before `Append`s** → `{Remove(X),
  Append(X')}` is a deterministic REPLACE (drop old, add new), order-independent.

```python
from artheia.manifest.algebra import Append, Remove, Explicit
from artheia.manifest.deployment import (
    DeploymentLayer, ExecutionLayer, ProcessLayer, MachineSetLayer, MachineLayer,
    ApplicationSetLayer, ApplicationLayer, ServiceLayer, ServiceInstanceLayer)

RIG = BASE.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={
        MachineLayer(name="central", arch=Explicit("aarch64"), cores={0,1,2,3}),
    }),
    execution=ExecutionLayer(processes={
        Remove(ProcessLayer(name="p1")),                 # drop a BASE process
        Append(ProcessLayer(name="tsync", machine=Explicit("central"))),
    }),
))
```

### Deleting from BASE — the gotcha

`Remove`-ing a process is not enough if other axes still reference it. Removing
process `p1` while leaving its `ServiceInstanceLayer` (whose `provided_by="p1"`)
orphans the service → `validate()` REFUSES to serialize. Remove all the pieces:

```python
# rpi4 service-test rig — drop the demo apps entirely (derive the names from the
# apps manifest so it tracks demo changes):
_APP_PROCS = {p.name for p in _members(APPS.execution.processes)}
_APP_SVCS  = [s.name for s in _members(APPS.service.instances)]
_APP_APPS  = [a.name for a in _members(APPS.applications.applications)]

RIG = BASE.combine(DeploymentLayer(
    execution=ExecutionLayer(processes={Remove(ProcessLayer(name=p)) for p in _APP_PROCS}
                              | {Append(ProcessLayer(name=n, machine=Explicit("central")))
                                 for n in _SVC_PROCS}),
    service=ServiceLayer(instances={Remove(ServiceInstanceLayer(name=s)) for s in _APP_SVCS}),
    applications=ApplicationSetLayer(applications={
        Append(ApplicationLayer(name="services", host_machine=Explicit("central"))),
        *(Remove(ApplicationLayer(name=a)) for a in _APP_APPS)}),
))
```

(`_members(layer.set_field)` folds a set field to its plain members — use it to
read what BASE contributed.)

## The rig shape

A rig module exports three names `serialize-manifest` reads:

```python
from manifest.assemble import BASE, BASE_SUPERVISORS, BASE_PROCESS_NODES, PROCESS_NAMES

RIG = BASE.combine(DeploymentLayer(...))   # the deployment (REQUIRED, via --attr)
SUPERVISORS = BASE_SUPERVISORS             # the supervision-tree metadata
PROCESS_NODES = BASE_PROCESS_NODES         # per-process node metadata (prune to match RIG)
```

- `BASE` = the assembled services ⊕ apps deployment (open machines — no host
  bound). A rig binds machines + processes and applies its deploy delta.
- If a rig drops processes from `RIG`, prune `PROCESS_NODES` to match (a dict
  comprehension filtering the removed names) so the executor tree has no
  dangling children.

## Validation (run automatically by `theia manifest`)

`validate(RIG)` (algebra.py) collects `Issue`s before serialize; any
`severity="error"` aborts. Cross-axis invariants (deployment.py `_invariants`):
process→declared-machine, cpu-affinity core exists, service `provided_by`
exists, app host + bundled processes resolve, `depends_on` resolves + acyclic,
TIPC-endpoint uniqueness (ERROR on distinct providers sharing an address),
empty-composition (WARNING). Fix errors in the rig, not by `--no-validate`.

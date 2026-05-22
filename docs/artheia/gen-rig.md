# Tutorial: bootstrap a vendor rig from `system.art`

`artheia gen-rig` writes a starter `rig.py` from a top-level
artheia composition. Use it to skip the boilerplate when standing
up a new vendor / vehicle / dev box rig.

## What you'll need

- An artheia `.art` file with a top-level `composition` declaration.
  Prototypes annotated with `on process <P>` group into process
  binaries.
- A target directory for the generated `rig.py` — by convention
  `<rig>/manifest/rig.py`.
- The `.venv/bin/artheia` CLI in your `$PATH` (or run via
  `.venv/bin/artheia ...`).

## End-to-end walkthrough — regenerate the demo

The demo rig is the canonical example. The composition lives at
`demo/system/demo/package.art`:

```
package system.demo

composition Demo3Way {
    prototype CounterNode     counter_p1      on process P1
    prototype DriverNode      driver_p1       on process P1
    prototype TickerNode      ticker_p1       on process P1
    prototype ObserverNode    observer_p2     on process P2
    prototype IncrementerNode incrementer_p3  on process P3
    connect driver_p1.inc_out         to counter_p1.inc_in
    connect driver_p1.counter_call    to counter_p1.srv
    connect observer_p2.counter_call  to counter_p1.srv
    connect incrementer_p3.inc_out    to counter_p1.inc_in
}
```

Five prototypes across three process groups (P1, P2, P3) — one
process binary per group, three SwComponents, three Executables,
three Processes.

### Step 1 — generate

```
artheia gen-rig demo/system/demo/package.art \
    --composition Demo3Way \
    --vehicle-name demo \
    --machine-name demo_host \
    --bazel-package //demo \
    --out demo/manifest/rig_generated.py
```

(For the demo, `--vehicle-name`, `--machine-name`, and
`--bazel-package` can be omitted — they default to values derived
from `--out`.)

Output: `demo/manifest/rig_generated.py`.

### Step 2 — inspect what it produced

The file is ~150 lines, all derived mechanically from the
composition + the convention defaults. Three blocks:

**Block 1 — host machine (you'll edit this)**:

```python
DemoHost = MachineManifest(
    name="demo_host",
    hardware=HardwareResource(
        cpu=CpuResource(architecture=CpuArchitecture.X86_64),  # TODO: real arch
    ),
    com_endpoint=IpEndpoint(
        address=IPv4Address("127.0.0.1"),                       # TODO: real IP
        port=7700,
    ),
)
```

`TODO:` markers flag deployment-specific values the generator
cannot infer. The defaults are sensible for local dev (loopback +
x86_64).

**Block 2 — process factories (don't edit; regenerated cleanly)**:

```python
_PROCESSES = [
    # (process_name, art_class, bazel_target, [prototype, ...])
    ('demo_p1', 'DemoP1Composition', '//demo:p1_main',
     ['counter_p1', 'driver_p1', 'ticker_p1']),
    ('demo_p2', 'DemoP2Composition', '//demo:p2_main', ['observer_p2']),
    ('demo_p3', 'DemoP3Composition', '//demo:p3_main', ['incrementer_p3']),
]


DEMO_COMPONENTS: list[SwComponent] = [
    SwComponent(name=name, bazel_target=target, owner="platform",
                art_node=f"system.demo/{art_class}")
    for (name, art_class, target, _) in _PROCESSES
]

# ... DEMO_EXECUTABLES, DEMO_PROCESSES via _executable_for, _process_for ...
```

**Block 3 — the structured-DSL spec layer (the interesting bit)**:

```python
DemoSpecLayer = SoftwareSpecification(
    vehicle=VehicleIdentity(name="demo", make="theia",
                            model="system.demo.Demo3Way"),
    machines=cast(set[SetTransformTypes], {Append(DemoHost)}),
    applications=cast(set[SetTransformTypes], {
        Append(ApplicationManifest(
            name="platform_app", host_machine=DemoHost.name,
            components=list(DEMO_COMPONENTS),
        )),
    }),
    execution_manifests=cast(set[SetTransformTypes], {
        Append(p) for p in DEMO_PROCESSES
    }),
)

DemoSoftware: SoftwareSpecification = FcSoftware.squash(DemoSpecLayer)
```

`FcSoftware` (from `services.manifest.fc`) is the platform base —
all 18 Adaptive Platform Functional Clusters plus their supervisor
tree. `DemoSpecLayer.squash` adds the demo machine, binds the
platform app to it, and overlays the three demo binaries.

### Step 3 — verify it works

```
artheia executor emit demo.manifest.rig_generated --out /tmp/demo_executor.yaml
```

Should print the output path and produce a 196-line `executor.yaml`
with the supervisor tree (root → ar_sup → core_sup → ... → app_sup
→ demo_p1/p2/p3 leaves).

```
artheia gui emit demo.manifest.rig_generated --out /tmp/demo_machines.yaml
```

Should produce:

```yaml
machines:
- name: demo_host
  address: 127.0.0.1
  port: 7700
```

If both commands work, the rig is plumbed correctly.

### Step 4 — fill in the TODO markers

Open the generated file and replace each `# TODO` comment with the
real value:

```python
DemoHost = MachineManifest(
    name="demo_host",
    hardware=HardwareResource(
        cpu=CpuResource(architecture=CpuArchitecture.AARCH64),  # was: X86_64
    ),
    com_endpoint=IpEndpoint(
        address=IPv4Address("10.0.0.42"),                       # was: 127.0.0.1
        port=7700,
    ),
)
```

This is now your hand-edited file. **Don't regenerate** — the
generator overwrites. Treat the generated rig.py as a starting
point, not a round-trip artifact.

### Step 5 — optional: per-process scheduling

Default `SchedulingPolicy.SCHED_OTHER` with priority 0 works for
most dev rigs. If a process needs realtime scheduling, add a
`NodeToCPUMapping` to the layer:

```python
from artheia.manifest.machine import NodeToCPUMapping, SchedulingPolicyEnum

_demo_p1_realtime = NodeToCPUMapping(
    name="demo_p1_realtime",
    process="demo_p1",
    scheduling_policy=SchedulingPolicyEnum.SCHED_FIFO,
    scheduling_priority=50,
    shall_run_on=["2", "3"],     # core IDs (as strings)
)

DemoSpecLayer = SoftwareSpecification(
    ...
    node_to_cpu_mappings=cast(set[SetTransformTypes], {
        Append(_demo_p1_realtime),
    }),
)
```

The supervisor honors `shall_run_on` (sets CPU affinity via
`sched_setaffinity`) and `scheduling_policy` / `scheduling_priority`
(via `sched_setscheduler`) when it spawns the process.

## CLI flags reference

```
artheia gen-rig <ART_FILE> [OPTIONS]

Required:
  -c, --composition <name>    Top-level composition name in <ART_FILE>.
  --out <path>                Where to write the generated rig.py.

Optional (with sensible defaults):
  --vehicle-name <str>        VehicleIdentity.name. Default: derived
                              from <path>'s parent dir name.
                              Example: demo/manifest/rig.py → "demo".
  --machine-name <str>        Default host machine name.
                              Default: "<vehicle>_host".
  --bazel-package <pkg>       Bazel package prefix for SwComponent
                              targets. Default: "//" + vehicle name.
                              Example: "//demo".
  --grpc-port <int>           services/com gRPC port the GUI connects
                              to. Default: 7700.
  --force                     Overwrite an existing non-empty file.
```

## What the generator does

1. Parses `<art_file>` via the artheia textX loader.
2. Finds the named composition; flattens it via
   `artheia.model.flatten_composition` (handles nested
   `composition Foo bar` references when cross-file imports are
   available — today, single-file compositions like Demo3Way work).
3. Groups prototypes by their `on process X` annotation in
   declaration order.
4. For each group, emits:
   - One `SwComponent` (name = `<vehicle>_<proc>`, bazel target =
     `<--bazel-package>:<proc>_main`).
   - One `Executable` (category APPLICATION_LEVEL, build type
     RELEASE — both overridable post-generation).
   - One `Process` (default `SCHED_OTHER`, priority 0 — adjust via
     `NodeToCPUMapping` if needed).
5. Emits a `<Vehicle>SpecLayer: SoftwareSpecification` declaring the
   host machine, the platform application (with all the per-process
   SwComponents), and the per-process Execution Manifest entries.
6. Emits `<Vehicle>Software = FcSoftware.squash(<Vehicle>SpecLayer)`
   — the final spec the CLI consumes.

## What the generator does NOT do

- Set realistic machine hardware / endpoints — `127.0.0.1` and
  `X86_64` are placeholders.
- Set per-process CPU affinity or realtime scheduling — defaults
  to `SCHED_OTHER`.
- Build a custom supervisor tree — inherits FcSoftware's OTP tree
  (root / ar_sup / core_sup / app_sup / etc).
- Resolve cross-file `import` in the .art (today single-file
  compositions only; see
  [`../tasks/TODO/system-art-aggregation.md`](../tasks/TODO/system-art-aggregation.md)).
- Round-trip your edits — once you edit the generated file,
  re-running the generator overwrites your changes.

All of these are explicit by design — the generator's job is
"derive the mechanical parts," not "infer deployment policy."

## Troubleshooting

**`composition X not found in <art_file>`**
The composition name is case-sensitive and must match the
declaration verbatim. Check with:

```
artheia parse <art_file>
```

— look for `CompositionDecl: <name>` in the output.

**`<out> exists and is non-empty`**
Pass `--force` to overwrite. **Don't do this if you've already
hand-edited the file** — your edits will be lost.

**`artheia executor emit` complains about missing rig**
The default auto-pick prefers `*Software` over `*Rig`. If your
generated file's export is named something else, pass it
explicitly:

```
artheia executor emit demo.manifest.rig_generated --rig DemoSoftware
```

**Generated `bazel_target` is wrong**
Pass `--bazel-package` to override. Example: a rig under
`vendor/myrig/` with binaries at `//vendor/myrig:p1_main`:

```
artheia gen-rig vendor/myrig/system/system.art \
    --composition MyComp \
    --bazel-package //vendor/myrig \
    --out vendor/myrig/manifest/rig.py
```

## Related

- [`manifest-dsl.md`](manifest-dsl.md) — manifest DSL reference.
- [`manifest.md`](manifest.md) — per-file module map.
- [`../tasks/DONE/generate-rig-from-system.md`](../tasks/DONE/generate-rig-from-system.md)
  — the spec ticket that drove `gen-rig`.

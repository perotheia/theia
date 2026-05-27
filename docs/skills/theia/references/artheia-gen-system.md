# Regenerating the system: .art → manifests → build → deploy

The chain that turns the `.art` model into running, packaged software.
Four stages: the **manifest python layer**, **serialization** to JSON, the
**Bazel build**, and **provisioning**. Plus the **AUTOSAR PSP** import chain
that feeds the gateway. All artheia commands assume
`PATH="$PWD/.venv/bin:$PATH"`.

```
 .art (system/, services/)
   │  load_platform_services(system/services)
   ▼
 manifest python layer ── services/manifest/service.py (FC catalog layer)
   │                       demo/manifest/rig.py        (the Demo3Way Rig)
   │  executor emit / gui emit / generate-manifest
   ▼
 serialized JSON ── executor.json, machines.yaml, dist/manifest/<machine>/*.json
   │  rig_ext (//rules:rig.bzl)
   ▼
 bazel build @rig_demo//… ── .ipk bundles; //:install runtime layout
   │  puppet / deploy_rpi4.sh
   ▼
 provisioned machines
```

## 1. The manifest python layer

The `.art` model is loaded into a Python object graph called a **Rig** (the
complete deployment: machines, applications, the FC service manifest, the
process/execution tree, and the supervisor hierarchy).

- **FC catalog** — `artheia/artheia/manifest/clusters.py` lists the 18 FCs
  (`CLUSTERS` / `BY_SHORT`). `loader.py:load_platform_services(art_root)`
  walks `<art_root>/<short>/package.art` for each, reading the atomic node's
  TIPC binding into a `LoadedFc`. The `art_root` defaults to
  **`system/services`** (`platform.py:_default_art_root` /
  `PLATFORM_SERVICES_ROOT`) — i.e. the aggregation symlink tree.
- **`PlatformBase`** (`manifest/platform.py`) — the base `Rig`: one
  application holding all FC components, the service manifest derived from
  the `.art` TIPC addresses, the per-FC processes, and the supervisor tree.
- **`services/manifest/service.py`** — the FC-layer + the hand-authored
  supervisor tree (`ar_sup`/`core_sup`/`network_sup`/`host_svc_sup`/
  `pltf_sup`/`app_sup`), with `shwa` pinned to the compute machine.
- **`demo/manifest/rig.py`** — the demo vehicle: composes layers into
  `Demo3Way`, then splits onto two target machines (central runs the
  platform services minus `shwa` + apps; compute runs `shwa` + its app).
  `demo/manifest/applications.py` is generated (`gen-manifest-proto`).

## 2. Serialization to JSON

| command | TARGET | emits |
| --- | --- | --- |
| `artheia executor emit <module> [--machine M] [--out f]` | dotted module path (e.g. `demo.manifest.rig`) | `executor.json` — the supervisor tree (whole-rig, or sliced to one machine) |
| `artheia gui emit <module> [--out f]` | same | `machines.yaml` — per-machine gRPC endpoints for the GUI |
| `artheia generate-manifest <module> [--out dist/manifest]` | same | per-machine `{machine,application,service,execution}.json` + `index.json` |

`TARGET` is a **Python import path**, not `module:attr`. Pick the Rig
attribute with `--rig` (defaults to `*Rig`/`Rig`). Examples:

```sh
artheia executor emit demo.manifest.rig                       # whole-rig tree → stdout
artheia executor emit demo.manifest.rig --machine compute_host --out central.json
artheia gui emit demo.manifest.rig --out machines.yaml
artheia generate-manifest demo.manifest.rig --out dist/manifest
```

The four per-machine manifests: **machine.json** (hw arch, os packages),
**application.json** (sw components on this machine), **service.json** (FC
instances pinned here), **execution.json** (processes, process→machine map,
supervisor slice, node→cpu mappings).

## 3. The Bazel build of manifests

`MODULE.bazel` declares a **module extension** that materializes a synthetic
repo per rig at module-resolution time:

```python
rig_ext = use_extension("//rules:rig.bzl", "rig_ext")
rig_ext.declare(name = "rig_demo", rig_module = "demo.manifest.rig")
use_repo(rig_ext, "rig_demo")
```

`rules/rig.bzl` runs `artheia rig-deps demo.manifest.rig`, reads the JSON,
and generates BUILD content. Resulting targets:

```sh
bazel build @rig_demo//:executor_json            # whole-rig executor.json
bazel build @rig_demo//:executor_json_central    # per-machine slices
bazel build @rig_demo//:machines_yaml            # GUI manifest
bazel build @rig_demo//<machine>:image           # the .ipk bundle
bazel build //system:art_sources                 # the .art filegroup the rig depends on
```

`demo/manifest:rig_sources` (in `demo/manifest/BUILD.bazel`) pulls
`//system:art_sources` + `//services/manifest:service_sources` so a `.art`
edit invalidates the manifest build. The top-level `//:install`
(`pkg_install`) lays the host artifacts into the runtime filesystem shape
(`theia/bin/`, `theia/lib/`, `etc/theia/`).

FC daemons and firmware build directly:

```sh
bazel build --config=linux //services/<fc>/main:<fc>
bazel build --config=ti_arm_cgt_18 //gateway/firmware/...     # Hercules TMS570
```

`.bazelrc` sets `--action_env=PATH` so artheia is found inside Bazel
actions. Do **not** commit `MODULE.bazel.lock`.

## 4. Provisioning & orchestration

A two-phase model under `deploy/`:

- **Provision** (`deploy/puppet/provisioning.pp`) — reads
  `/etc/theia/manifest/machine.json`, installs OS packages + the `.ipk`
  artifacts (supervisor, gateway), restarts the stack. *Framework exists;
  the package-install body is still a stub.*
- **Orchestrate** — day-to-day app updates without a supervisor restart.

What's **real and end-to-end today**: `tools/deploy_rpi4.sh` (cross-build →
`scp` → `dpkg -i` to a Pi 4, with a TIPC `modprobe` sanity check), and the
`deploy/` docker-compose stack (central + compute containers + etcd). Treat
the Puppet path as aspirational scaffolding until its body lands.

## 5. The AUTOSAR PSP chain (gateway)

Independent of the FC pipeline: turns vendor bus descriptions into the
gateway's codec + routing. All steps are real and idempotent.

```
DBC / FIBEX  ──import-dbc / import-fibex──►  package.art + catalog.json   (per bus)
catalog.json ──gen-autosar-system──►         system.art  (one mega-node per bus)
catalog.json ──gen-psp-netgraph──►           netgraph.json  (PDU → bus address)
```

```sh
PSP=autosar/mlbevo_gen2_cmp_psp
# CAN
artheia import-dbc  --dbc $PSP/config/dbc/*KCAN*.dbc --bus kcan \
    --out $PSP/system/kcan --package autosar.mlbevo_gen2_cmp_psp.system
# FlexRay (slow; --no-validate skips the post-import round-trip)
artheia import-fibex --fibex $PSP/config/MLBevo_Gen2_Fx_Cluster_*.xml --bus mlbevo_gen2 \
    --out $PSP/system/mlbevo_gen2 --package autosar.mlbevo_gen2_cmp_psp.system --no-validate
# aggregate
artheia gen-autosar-system --catalog $PSP/system/kcan/catalog.json \
    --catalog $PSP/system/mlbevo_gen2/catalog.json \
    --out $PSP/system/system.art --package autosar.mlbevo_gen2_cmp_psp.system
```

A FIBEX-scale import (1000s of PDUs) takes minutes; `--no-validate` skips
the round-trip parse (safe — the catalog schema is well-tested and the
emitter is deterministic). Generated `.art` carries an `AUTO-GENERATED`
header; never hand-edit — re-run the importer.

## The full command surface

`artheia --help` lists everything. The ones above are the load-bearing
system-regen commands; others you'll meet: `parse`, `gen-netgraph` /
`gen-host-netgraph` (symbolic port → TIPC LUT), `gen-proto` /
`gen-manifest-proto`, `gen-trace-decoder-subset`, `gen-rig` (bootstrap a
vendor `rig.py`), `audit-manifest` (`.art` ↔ `rig.py` drift check),
`rig-deps` (the JSON the Bazel extension consumes).

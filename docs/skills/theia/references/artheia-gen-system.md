# Regenerating the system: .art → manifests → build → deploy

The chain that turns the `.art` model into running, packaged software.
This page covers the **first two stages** — the manifest python layer
and serialization to JSON. Bazel, provisioning, and the AUTOSAR PSP
chain each have their own page:

| stage | covered in |
| --- | --- |
| 1. manifest python layer | this page §1 |
| 2. serialization to JSON | this page §2 |
| 3. Bazel build of manifests + FCs | [build-system.md](build-system.md) |
| 4. provisioning & orchestration | [provision-orchestrate.md](provision-orchestrate.md) |
| AUTOSAR PSP import (independent) | [autosar.md](autosar.md) |

All artheia commands assume `PATH="$PWD/.venv/bin:$PATH"`.

```
 .art (system/, services/)
   │  load_platform_services(system/services)
   ▼
 manifest python layer ── services/manifest/service.py (FC catalog layer)
   │                       demo/manifest/rig.py        (the Demo3Way Rig)
   │  executor emit / gui emit / generate-manifest
   ▼
 serialized JSON ── executor.json, machines.yaml, dist/manifest/<machine>/*.json
   │  rig_ext (//rules:rig.bzl)                          → build-system.md
   ▼
 bazel build @rig_apps//… ── .ipk bundles; //:install runtime layout
   │  puppet / deploy_rpi4.sh                            → provision-orchestrate.md
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
| `artheia executor emit <module> [--machine M] [--out f]` | dotted module path (e.g. `apps.manifest.rig`) | `executor.json` — the supervisor tree (whole-rig, or sliced to one machine) |
| `artheia gui emit <module> [--out f]` | same | `machines.yaml` — per-machine gRPC endpoints for the GUI |
| `artheia generate-manifest <module> [--out dist/manifest]` | same | per-machine `{machine,application,service,execution}.json` + `index.json` |

`TARGET` is a **Python import path**, not `module:attr`. Pick the Rig
attribute with `--rig` (defaults to `*Rig`/`Rig`). Examples:

```sh
artheia executor emit apps.manifest.rig                       # whole-rig tree → stdout
artheia executor emit apps.manifest.rig --machine compute_host --out central.json
artheia gui emit apps.manifest.rig --out machines.yaml
artheia generate-manifest apps.manifest.rig --out dist/manifest
```

The four per-machine manifests: **machine.json** (hw arch, os packages),
**application.json** (sw components on this machine), **service.json** (FC
instances pinned here), **execution.json** (processes, process→machine map,
supervisor slice, node→cpu mappings).

These three commands are what `rules/rig.bzl` shells out to under the hood
(via `artheia rig-deps`) — but you can run them by hand to inspect a rig
without touching Bazel. From here the chain continues into
[build-system.md](build-system.md).

## The full command surface

`artheia --help` lists everything. The system-regen commands above are
load-bearing; others you'll meet:

| command | purpose |
| --- | --- |
| `parse` | full-tree resolve (validation) — `artheia parse system/system.art` |
| `gen-netgraph` | nodes + compositions → JSON netgraph (symbolic port → TIPC LUT) |
| `gen-proto` | `.proto` emission from `.art` (one file per message; the supervisor's CMake re-runs it for its standalone proto) |
| `gen-manifest` | system `.art` → the FC manifest **Python** module (`services/manifest/service.py`); not a `.proto` |
| `gen-app` | the C++ app scaffold (lib + main + impl) — the single C++-from-`.art` path |
| `gen-rig` | bootstrap a vendor `rig.py` from a top-level `.art` |
| `audit-manifest` | `.art` ↔ `rig.py` drift check |
| `rig-deps` | the JSON the Bazel rig extension consumes |
| `gen-etcd` | etcd seed schema for all node params |

Retired (do not look for them): `gen-host-netgraph` (use `gen-netgraph`),
`gen-proto-package` (gen-app emits per-package `.proto` internally),
`gen-cpp-stubs` (conflicted with `gen-app`, which emits the
GenServer/GenStateM daemon directly), `gen-trace-decoder-subset` (unused; the
trace decoder is built as a dependency).

For the AUTOSAR PSP-side generators (`gen-can-codec`,
`gen-fibex-codec`, `gen-psp-netgraph`, etc.), see
[autosar.md](autosar.md).

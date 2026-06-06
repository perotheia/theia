# S2 — flexraya_world_source integration map

What `vendor/odd_path_monitor` does on branch `flexraya_world_source`
(160 files, ~13.7k new lines vs `theia/main`), and what the new
`gen-app --kind lib` arm must provide so S5 can replace it.

Subordinate to [psp-retirement.md](psp-retirement.md).

## TL;DR of the old approach

App-private `platform/` directory wraps **`libpsp.so` (dlopen'd at
runtime)** + a custom dispatch table + a Hercules slot filter, all
generated from one `signals.csv` wishlist via `platform/generate.sh`.
The app links a static `platform_dispatch` library and links/dlopens
`libpsp.so` for the encoders. `FlexRayCmpAdapter` glues decoded nanopb
PDUs into the `reconstruct::InputFrame` the BEV pipeline consumes.

**The seam to preserve:** `FlexRayWorldSource → FlexRayCmpAdapter → reconstruct::InputFrame`. Everything above that line is application
code (BEV, RSS, UI, recording). Everything below it is what `--kind lib` must re-emit in the new shape.

## The seam in pictures

```
                Ethernet (CMP/UDP from Hercules)
                          │
                          ▼
              ┌──────────────────────┐
              │  pero_cmp_lnx        │     (existing host-side
              │  CmpReceiver         │      ASAM-CMP decoder lib;
              └──────────┬───────────┘      stays — vendored in?)
                         │ (slot, ch, raw PDU bytes)
                         ▼
              ┌──────────────────────┐
              │ platform/generated/  │     ← THIS is the surface
              │ dispatch_table.{h,c} │       gen-app --kind lib
              │ hercules_filter.h    │       must produce
              │ ns_wrapper.h         │
              │ <PDU>.pb.{h,c}       │
              └──────────┬───────────┘
                         │ pb_decode → typed nanopb structs
                         ▼
              ┌──────────────────────┐
              │ FlexRayCmpAdapter    │     APP CODE — keep as-is,
              │ (app/core/reconstruct│     but the headers it
              │  /FlexRayCmpAdapter) │     #includes change to the
              └──────────┬───────────┘     gen-app outputs.
                         │ InputFrame
                         ▼
              ┌──────────────────────┐
              │ Reconstructor → BEV  │     APP CODE — untouched.
              └──────────────────────┘
```

## Files added on the branch — categorized

### A. The PSP artefact directory — what `--kind lib` will replace

| file | what it does | new-arm equivalent |
| --- | --- | --- |
| `platform/config/signals.csv` | hand-edited 200+ row wishlist of (signal, PDU, slot, channel) tuples | move into `vendor/<app>/system/<app>/component.art` as port + interface declarations (each PDU is a `message`, each FlexRay slot mapping is a `gateway_route`) |
| `platform/generate.sh` | 5-phase pipeline: validate → ensure PSP → ensure libpsp → gen_app_dispatch → expand_dispatch → nanopb | folded into `artheia gen-app --kind lib` (one command) |
| `platform/generated/dispatch_table.{h,c}` | `(slot, channel) → cmp_encode_*` C dispatch | emitted by `--kind lib`; same shape but from `.art` `gateway_route` blocks |
| `platform/generated/hercules_filter.h` | constexpr slot allowlist for HerculesControl | emitted by `--kind lib`; derived from the union of `gateway_route` slots |
| `platform/generated/ns_wrapper.h` | C++ typedefs into the namespaced nanopb types | emitted by `--kind lib` (or made unnecessary by namespacing the .pb.h's directly) |
| `platform/generated/<PDU>.pb.{h,c}` (40+ files) | nanopb proto compilation outputs | emitted by `artheia gen-proto` + nanopb, copied into `<app>/platform/generated/` by `--kind lib`'s vendoring step |
| `platform/CMakeLists.txt` | exports `platform_dispatch` static lib | emitted by `--kind lib` (template, not hand-written) |
| `platform/README.md` | documents the pipeline | kept; describe the new (single-command) regen flow instead |
| `platform/tools/expand_dispatch.py` | post-processes upstream `gen_app_dispatch.py` to fix two upstream bugs (one-(slot,ch)-per-PDU, missing 9th struct field) | **subsumed** by `--kind lib`'s native multi-slot support; this script ceases to exist |
| `platform/tools/validate_signals.py` | CI-friendly check that signals.csv resolves against the FIBEX | **moves into artheia** as part of `gen-app --kind lib`'s validation pass (or stays as an `artheia validate-app-signals` subcommand) |

### B. App-side glue — what stays, with header rewires

| file | role | S5 change |
| --- | --- | --- |
| `app/core/world/FlexRayWorldSource.{h,cpp}` | Qt-side world source, owns the `CmpReceiver`, drives `FlexRayCmpAdapter`. Live + replay modes. | Keep wholesale. Only `start()` signature might shift if PSP discovery changes. |
| `app/core/world/FlexRayWorldSource.cpp` (581-line monster) | implements live + pcap-replay receive threads | Keep; the receive loop is gateway-side detail and lives in vendor code now too |
| `app/core/reconstruct/FlexRayCmpAdapter.{h,cpp}` | converts decoded nanopb PDUs → `reconstruct::InputFrame` | Keep. Update `#include` paths to point at the new generated location (`<app>/platform/generated/...` stays the same path → no change probable) |
| `app/core/reconstruct/FlexRaySignals.h` | enums + raw signal types | Keep |
| `app/core/reconstruct/Reconstructor.{h,cpp}` | BEV scene rebuild | Keep — pure app code |
| `app/core/reconstruct/{ObjectFilter,PolylineBuilder}.{h,cpp}` | sub-pipelines | Keep — pure app code |

### C. AppController wiring

`app/core/AppController.cpp` gains a `RSS_WORLD_SOURCE=flexray` branch
that constructs `FlexRayWorldSource`, reads four env vars:

```
RSS_FLEXRAY_IFACE   — Ethernet iface where Hercules sends CMP
RSS_FLEXRAY_PCAP    — pcap file for offline replay (wins over IFACE)
RSS_PSP_ROOT        — path to mlbevo_gen2_cmp_psp (for dlopen libpsp.so)
RSS_HERCULES_IP     — control-plane IP for Hercules slot filter push
```

**S5 implication:** the old approach treats the PSP as an external
artifact (`RSS_PSP_ROOT` must point at `mlbevo_gen2_cmp_psp/build/`).
The new approach must either:

- **(a)** Keep `RSS_PSP_ROOT` but have artheia stage `libpsp.so` into
  the standalone tree (so the env var points inside `<app>/`), OR
- **(b)** Replace dlopen+libpsp with statically-linked encoders
  emitted into `<app>/platform/generated/` so no PSP_ROOT is needed.

Option (b) is the natural endpoint of "self-sufficient under CMake".

### D. Unrelated noise on the branch (do NOT pull into S5)

These files are independent of the FlexRay integration and should be
filtered out of any "what S5 needs to handle" analysis:

- `app/core/connectivity/{F9rSettings,RtklibReceiver,UbxConfigEncoder,ZbusSubscriber}.{h,cpp}` — RTKlib / GNSS support
- `app/core/influx/InfluxSettings.{h,cpp}` — InfluxDB telemetry settings
- `app/core/recording/{DcimPath,ExifJpegWriter,VideoSettings}.{h,cpp}` + VideoRecorder rework — camera capture / DCIM JPEG dump
- `app/core/world/CarlaWorldSource.{h,cpp}` — CARLA dev source (parallel to FlexRay, not replacing it)
- `app/ui/qml/screens/SettingsScreen.qml` + `app/ui/qml/components/BevView.qml` reworks — UI
- All `docs/catalog/*/scenario.md`, `tasks/*` — docs + planning notes
- `dev/docker-compose.yml`, `launch.sh` — dev infra

## The dispatch table shape (so S4 knows the target)

`platform/generated/dispatch_table.c` is the load-bearing artifact.
Its row type comes from `pero_cmp_lnx/lib/include/cmp_plugin.h`:

```c
typedef struct {
    uint16_t slot;
    uint8_t  channel;
    const char *pdu_name;
    cmp_encode_fn encode;       // resolved at runtime via PspLoader
    cmp_decode_fn decode;       // currently NULL (decode handled via nanopb)
    size_t pdu_byte_offset;
    size_t pdu_byte_count;
    uint8_t cycle_mask;
    uint8_t cycle_repetition;
} cmp_dispatch_entry_t;
```

Each row corresponds to **one (slot, channel) tuple** the application
listens to. The current dispatch_table.c has ~50 rows for the 18 PDU
types odd-path-monitor consumes (some PDUs appear on multiple slots —
`expand_dispatch.py`'s reason for existing).

`--kind lib` must:

1. Walk every `gateway_route <NodeName> { fr slot=... bus=... }` in the
   app's `component.art`.
2. Take the union of (slot, channel) tuples per PDU type.
3. Emit one `cmp_dispatch_entry_t` per tuple.
4. Cross-reference the PDU's nanopb-generated `_fields[]` array name
   for the decode pointer (or leave NULL if decode is still via
   pb_decode in the adapter — matches today's behavior).
5. Emit the Hercules slot allowlist as a constexpr in
   `hercules_filter.h`.

## What this means for S3 (the `.art` spec)

The `.art` for `odd-path-monitor` needs to declare:

```scala
package vendor.odd_path_monitor

// One message per PDU — these become the nanopb types in
// <app>/platform/generated/<PDU>.pb.{h,c}
message EML_01 { … }
message BV2_ObjektHeader { … }
message BV2_Objekt_01 { … }
… // 18 messages total

// One node per logical consumer. There's likely just one — the
// FlexRayCmpAdapter — that subscribes to all of them.
node atomic FlexRayIngress {
    tipc type=0x… instance=0
    fallthrough = false             // production node
    ports {
        // One receiver port per PDU type, requires the matching
        // sender interface in the gateway service spec.
        receiver eml_01            requires EML_01_Stream      best_effort
        receiver bv2_objekt_header requires BV2_ObjektHeader_Stream best_effort
        … // 18 receivers
    }
}

// One gateway_route per (PDU, slot, channel) tuple.
gateway_route FlexRayIngress {
    fr slot=5  channel=0 bus=mlbevo_gen2  pdu=EML_01
    fr slot=8  channel=0 bus=mlbevo_gen2  pdu=Licht_hinten_01
    fr slot=76 channel=0 bus=mlbevo_gen2  pdu=BV2_SensorHeader
    fr slot=77 channel=1 bus=mlbevo_gen2  pdu=BV2_Objekt_06     // dual-slot example
    fr slot=78 channel=0 bus=mlbevo_gen2  pdu=BV2_Objekt_06
    … // ~50 routes total (union of all (slot,ch) per PDU)
}
```

NB: the **current grammar** at `artheia/artheia/grammar/artheia.tx`
has `gateway_route` (line 196 in our docs) but its body shape today is
`can id=... bus=... dlc=...` — there's likely no FlexRay
(`fr slot=... channel=... bus=... pdu=...`) form yet. S4 will need to
extend the grammar with the FlexRay route shape, or we co-opt the
existing CAN form to a unified bus-agnostic route shape.

## What this means for S4 (the generator)

`artheia gen-app --kind lib <component.art>` must produce, into
`<app>/platform/`:

| output | from |
| --- | --- |
| `lib/<Node>.hh` + `impl/<Node>_handlers.cc` | `node` declarations (same as `--kind fc`) |
| `generated/<PDU>.pb.{h,c}` | `message` declarations → `gen-proto-package` → nanopb |
| `generated/dispatch_table.{h,c}` | `gateway_route` declarations (FlexRay form) |
| `generated/hercules_filter.h` | union of slots from `gateway_route` declarations |
| `generated/ns_wrapper.h` | per-PDU C++ typedefs (`namespace odd_path_monitor { using EML_01 = pb_EML_01; }`) |
| `CMakeLists.txt` | template — declares `platform_dispatch` static lib + nanopb deps |
| `runtime/` (subset) | copied from `platform/runtime/` — only the headers the app's `GenServer` nodes use (`GenServer.hh`, `TipcMux.hh`, transport bits) |
| `runtime/proto/` | the `.proto` set the app's nodes' TIPC messages need, nanopb-compiled |

The directory layout in the new world:

```
vendor/odd_path_monitor/
├── app/                          (existing, untouched)
├── platform/
│   ├── CMakeLists.txt            (gen-app --kind lib)
│   ├── lib/<Node>.hh             ┐
│   ├── impl/<Node>_handlers.cc   ├─ from .art `node` decls
│   ├── generated/
│   │   ├── <PDU>.pb.{h,c}        ┐
│   │   ├── dispatch_table.{h,c}  ├─ from .art `gateway_route` decls
│   │   └── hercules_filter.h     ┘
│   ├── runtime/                  copied from platform/runtime/ (Theia)
│   │   ├── GenServer.hh
│   │   ├── TipcMux.hh
│   │   └── …
│   └── runtime/proto/            nanopb-compiled Theia internals
└── system/odd_path_monitor/
    ├── package.art               messages + nodes
    └── component.art             composition + gateway_route
```

## Open seams (decide during S4)

1. **`libpsp.so` vs static link.** The current adapter dlopens
   `libpsp.so` at runtime (encoders for ~1400 upstream PDUs; only 18
   used). The new arm should statically link only the encoders we
   need — drops the `RSS_PSP_ROOT` env requirement.
2. **`pero_cmp_lnx` headers** (`cmp_plugin.h`). Today's
   `platform/CMakeLists.txt` expects `PERO_CMP_LNX` to point at the
   sibling repo. New arm: vendor the needed headers into
   `<app>/platform/include/` so the app builds standalone.
3. **FIBEX validation.** `validate_signals.py` checks the wishlist
   against the FIBEX. New shape: move into artheia as a check that
   every `gateway_route` resolves against the bus's catalog.json.
4. **`gateway_route` FlexRay form.** Grammar extension needed (today
   only `can id=...` is in the spec; FlexRay needs `slot/channel/pdu`).
5. **Multi-PDU-per-slot.** `expand_dispatch.py`'s reason for existing
   — `--kind lib` needs to handle this natively (one PDU appears in
   multiple `gateway_route` entries, all kept).

## What I touched in S2

Read-only on the vendor repo. The flexraya_world_source branch is
checked out in `vendor/odd_path_monitor/` for follow-up reference;
S5 will diff against `theia/main` (HEAD of the standalone-app
direction) to extract what to keep vs. what to replace.

This integration map document is the deliverable of S2.
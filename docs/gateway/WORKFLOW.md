# Platform CMP Workflow — Repo Organisation & Recommendations

## Target structure

```
ccstheia/
├── pero_cmp_ti/              Hercules firmware (ASAM-CMP capture layer)
├── pero_cmp_lnx/             pero_cmp framework (generators + runtime lib)
├── MLBevo_Gen2_cmp_psp/      Platform Support Package  ← NEW
└── MLBevo_Gen2_cmp_demo/     Demo application           ← NEW
```

---

## Repo responsibilities

### pero_cmp_ti  (Hercules firmware)
- FlexRay + CAN capture, ASAM-CMP framing
- Slot filter control (port 5001)
- Time-sync receiver (TIME_SYNC opcode 0x05)
- **No changes needed for the PSP/demo split**

---

### pero_cmp_lnx  (framework)

**Stays in this repo:**

| Path | What it is |
|---|---|
| `lib/` | `libcmpdecoder.a` — ASAM-CMP receive, pcap, FIBEX/DBC parser |
| `lib/include/cmp_plugin.h` | Stable ABI for generated codec .so |
| `lib/include/cmp_extract.h` | Header-only bit extraction (used by generated C) |
| `lib/include/cmp_proto_wire.h` | Header-only proto3 wire encoder (used by generated C) |
| `lib/include/hercules_control.h` | Filter + time-sync push API |
| `lib/include/time_sync_pusher.h` | Exponential backoff pusher |
| `generator/asam_cmp_parser.py` | Unified FibexDb + DbcDb + PlatformDb |
| `generator/fibex_to_nanopb.py` | FlexRay PDU → proto3 generator |
| `generator/can_to_nanopb.py` | CAN message → proto3 generator |
| `demo/timesync/` | Framework smoke-test (keep) |

**Moves out:**

| From | To | Why |
|---|---|---|
| `configs/` | `MLBevo_Gen2_cmp_psp/config/` | Config is platform data, not framework |
| `app_example/` | `MLBevo_Gen2_cmp_demo/` | Application code belongs in the app repo |
| `test/adas_subset_v1.csv` | `MLBevo_Gen2_cmp_psp/config/` | Platform signal selection |
| `test/can_subset_kcan.csv` | `MLBevo_Gen2_cmp_psp/config/` | Platform signal selection |

**Missing — needs to be added:**

| Item | Description |
|---|---|
| `lib/include/psp_loader.h` | C++ class that `dlopen`s `$PSP_ROOT/libpsp.so`, exposes dispatch tables and filter constants |
| `generator/gen_app_dispatch.py` | NEW: given `--psp-root` + app CSV → generates dispatch table (no codecs, those are in libpsp.so), filter header, namespace wrappers |
| Generator `--all-signals` flag | When generating the PSP we want ALL PDUs/messages, not a subset; avoids maintaining a "select everything" CSV |
| `CMakeLists.txt` install target | `cmake --install` publishes headers + libcmpdecoder to a sysroot so PSP/demo can find it without knowing the source path |

---

### MLBevo_Gen2_cmp_psp  (Platform Support Package)

**Purpose**: single artifact representing the full signal codec for one platform version.  
`libpsp.so` encodes ANY FlexRay PDU or CAN message defined in the K-Matrix.

**Directory layout:**

```
MLBevo_Gen2_cmp_psp/
├── config/
│   ├── MLBevo_Gen2_Fx_Cluster_...xml      FlexRay FIBEX
│   └── dbc/
│       ├── MLBevo_..._KCAN_...dbc
│       ├── MLBevo_..._HCAN_...dbc
│       ├── MLBevo_..._ICAN_...dbc
│       └── ...
├── proto/                               generated .proto + .pb.h/.pb.c  ← COMMITTED
│   ├── flexray/                         one per FlexRay PDU
│   └── can/<bus>/                       one per CAN message per bus
├── src/                                 generated encoder .c files     ← COMMITTED
│   ├── flexray/
│   └── can/<bus>/
├── include/
│   └── psp_manifest.h                   constexpr: version, bus names, slot list
├── CMakeLists.txt                        builds libpsp.so
├── psp_manifest.json                    records FIBEX/DBC paths + versions
├── generate.sh                          runs generators → fills proto/ + src/
└── .gitignore                           build/, libpsp.so
```

**`generate.sh` workflow:**

```sh
#!/bin/sh
# Regenerate proto/ and src/ from config/
PERO_CMP_GEN=$PERO_CMP_LNX_ROOT/generator

python3 $PERO_CMP_GEN/fibex_to_nanopb.py \
    --fibex config/MLBevo_Gen2_Fx_Cluster_...xml \
    --namespace mlbevo_gen2 --all-signals \
    --out src/flexray/

for DBC in config/dbc/*.dbc; do
    BUS=$(basename $DBC | grep -oP 'M?LBevo_\w+_\K\w+CAN' | tr '[:upper:]' '[:lower:]')
    python3 $PERO_CMP_GEN/can_to_nanopb.py \
        --dbc $DBC --namespace can_$BUS --all-signals \
        --out src/can/$BUS/
done

protoc --nanopb_out=proto/ src/**/*.proto
```

**`CMakeLists.txt`:**
```cmake
# libpsp.so = encode functions + dispatch tables ONLY.
# NO *.pb.c here — libpsp has no nanopb dependency.
# The encode functions write proto3 wire bytes using cmp_proto_wire.h
# (header-only inline).  pb_decode() lives in the APP, not here.
add_library(psp SHARED
    src/flexray/encode_*.c         # FlexRay PDU → proto wire
    src/can/kcan/can_encode_*.c    # CAN message → proto wire (per bus)
    src/flexray/dispatch_table.c   # slot+channel → encode fn pointer
    src/can/kcan/can_dispatch_table.c
)
target_include_directories(psp PRIVATE ${PERO_CMP_LNX_INCLUDE})
# cmp_extract.h + cmp_proto_wire.h are header-only — no lib link needed
```

`*.pb.c` (nanopb decode runtime) belongs in the **application**, not libpsp.so.

**Key point**: `libpsp.so` has **zero runtime deps** beyond libc.

```
libpsp.so encode path:
  FlexRay/CAN bytes → cmp_extract.h (inline) → cmp_proto_wire.h (inline) → proto3 wire []uint8

Application decode path:
  proto3 wire []uint8 → pb_decode() (nanopb, *.pb.c) → POD struct (*.pb.h)
```

---

### MLBevo_Gen2_cmp_demo  (Demo Application)

**Purpose**: shows how an application uses the framework + PSP at runtime.  
Each app chooses its signal subset via a CSV; the framework generates app-specific glue code.

**Directory layout:**

```
MLBevo_Gen2_cmp_demo/
├── config/
│   └── demo_signals.csv          signal subset the app cares about
├── protos/                        GENERATED + COMMITTED app glue
│   ├── dispatch_table.h           points into libpsp.so encode functions
│   ├── hercules_filter.h          constexpr slot + CAN-ID lists
│   ├── ns_wrapper.h               C++ namespace typedefs
│   └── <selected>.pb.h/.pb.c     nanopb POD structs (subset)
├── src/
│   └── main.cpp
├── CMakeLists.txt
└── generate.sh                    runs gen_app_dispatch.py → fills protos/
```

**`generate.sh`:**
```sh
python3 $PERO_CMP_LNX_ROOT/generator/gen_app_dispatch.py \
    --psp-root  $PSP_ROOT \
    --csv       config/demo_signals.csv \
    --out       protos/
```

**`main.cpp` runtime usage:**
```cpp
#include "psp_loader.h"          // pero_cmp_lnx
#include "hercules_control.h"
#include "time_sync_pusher.h"
#include "dispatch_table.h"      // generated for this app
#include "ns_wrapper.h"

int main() {
    // 1. Load PSP
    PspLoader psp;
    psp.load(getenv("PSP_ROOT"));   // dlopen libpsp.so, wire dispatch table

    // 2. Configure Hercules filter
    HerculesControl ctrl;
    ctrl.open("169.254.8.3");
    ctrl.send_filter(kHerculesFilterSlots, kHerculesFilterSlotCount);

    // 3. Start time sync
    TimeSyncPusher sync;
    sync.start(ctrl);

    // 4. Receive + decode
    CmpReceiver rx;
    rx.open("enp132s0");
    uint8_t buf[1024];
    for (;;) {
        ssize_t n = rx.recv_packet(buf, sizeof(buf));
        auto* e = cmp_lookup(slot_id, ch);   // from dispatch_table.h
        uint8_t proto_buf[e->proto_buf_max];
        e->encode(pdu, pdu_len, proto_buf, sizeof(proto_buf));
        // pb_decode → POD struct → app logic
    }
}
```

---

## Data flow

```
K-Matrix FIBEX / DBC              PSP_ROOT env var
      │                                  │
      ▼                                  ▼
generate.sh (pero_cmp_lnx generators)  PspLoader::load()
      │                               dlopen libpsp.so
      ▼                               wire cmp_dispatch_table
  libpsp.so  ◄─── linked at build ───  dispatch_table.h (app-specific)
      │
      │  encode FlexRay/CAN bytes → proto3 wire buffer
      ▼
  pb_decode() + nanopb POD structs  ◄── protos/*.pb.h (app-specific)
      │
      ▼
  Application logic
```

---

## Inconsistencies in the proposed design

| # | Issue | Recommendation |
|---|---|---|
| 1 | Generators currently produce codec C files AND dispatch table together | Split: PSP generator produces codecs (→ libpsp.so), `gen_app_dispatch.py` produces only dispatch table + headers per app |
| 2 | No `PspLoader` class exists yet | Add `lib/include/psp_loader.h` + `lib/src/psp_loader.cpp` to pero_cmp_lnx |
| 3 | No `gen_app_dispatch.py` exists | Write this script; it reads PSP's `psp_manifest.json` to find available codecs and maps the app CSV to dispatch entries |
| 4 | `--all-signals` not supported by generators | Add flag; without it the PSP build requires a massive "everything" CSV |
| 5 | `libpsp.so` symbol naming: codecs are `cmp_encode_ACC_06` but multiple buses could have same name | Prefix per namespace: `cmp_encode_mlbevo_gen2_ACC_06`, `cmp_can_encode_can_kcan_ACC_07` |
| 6 | `app_example/protos/` are hardcoded to `mlbevo_v8_17` | These become per-app generated files in `MLBevo_Gen2_cmp_demo/protos/` |
| 7 | pero_cmp_lnx `configs/` mixed with framework code | Move to PSP repo; pero_cmp_lnx tests should reference PSP root via env var |
| 8 | PSP namespace vs version: current namespace is `mlbevo_v8_17` (version-suffixed) | PSP namespace = platform name only (`mlbevo_gen2`); version is a PSP repo tag, not baked into symbol names |
| 9 | No `psp_manifest.json` | PSP generator should write a manifest recording which FIBEX/DBC files were used, their checksums, and the list of generated codecs — `gen_app_dispatch.py` reads this to validate that the app CSV references real PSP signals |
| 10 | No install target in pero_cmp_lnx CMake | Add `install(TARGETS cmpdecoder DESTINATION lib)` + `install(DIRECTORY lib/include/ DESTINATION include/pero_cmp)` so PSP/demo CMake can do `find_package(PeroCmp)` |

---

## Missing pieces — action list

Priority order:

1. **`PspLoader`** (pero_cmp_lnx) — `dlopen` + dispatch table wiring; needed by demo runtime
2. **`--all-signals` flag** on both generators — needed for PSP build without a "select all" CSV
3. **`gen_app_dispatch.py`** (pero_cmp_lnx/generator/) — generates per-app dispatch from PSP manifest
4. **Symbol prefixing** — rename `cmp_encode_<PDU>` to `cmp_encode_<namespace>_<PDU>` in generated .c + dispatch table
5. **`psp_manifest.json` writer** in generators — records FIBEX/DBC → codec mapping
6. **`generate.sh`** + `CMakeLists.txt` in PSP repo
7. **Repo skeleton creation** — `MLBevo_Gen2_cmp_psp/` and `MLBevo_Gen2_cmp_demo/`
8. **Move `configs/`** from pero_cmp_lnx to PSP repo (update pero_cmp_lnx tests to use `$PSP_ROOT`)
9. **CMake install target** in pero_cmp_lnx

---

## Boundary summary

| What belongs where | pero_cmp_lnx | MLBevo_Gen2_cmp_psp | MLBevo_Gen2_cmp_demo |
|---|---|---|---|
| Framework libs (libcmpdecoder) | ✓ | | |
| Generator scripts | ✓ | | |
| FIBEX + DBC config files | | ✓ | |
| Proto encoders (libpsp.so) | | ✓ generated | |
| nanopb POD headers (all signals) | | ✓ generated | |
| App signal CSV | | | ✓ |
| App dispatch table | | | ✓ generated |
| App nanopb POD headers (subset) | | | ✓ copied from PSP |
| App main.cpp | | | ✓ |
| Hercules filter API | ✓ | | |
| Time sync API | ✓ | | |
| PspLoader | ✓ | | |

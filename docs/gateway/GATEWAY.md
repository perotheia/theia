# Gateway Architecture

This document describes the bidirectional CAN/FlexRay gateway that bridges
Hercules-based vehicle network capture (pero_cmp_gw firmware) to Linux
host applications via TIPC sockets.

---

## 1. System Overview

```
┌─────────────────────────────── pero_cmp_gw (TMS570LS3137, Hercules) ──────────────────────────────────┐
│                                                                                                         │
│  CAN1-4 ── ISR ──► onCanDataReceived()                                                                 │
│                        │ filter: cmp_can_id_is_accepted(can_id)                                        │
│                        ▼                                                                                │
│                    populateCmpCanFrame()  ──►  UDP 5010-5020 (ASAM-CMP capture)                        │
│                                                                                                         │
│  FlexRay ── poll ──► receiveFlexrayAndSendUdp()                                                        │
│                        │ populateCmpFlexRayFrame()  ──► UDP 5010-5020                                  │
│                                                                                                         │
│  UDP port 5021 ◄── gw_tx_recv_callback()                                                               │
│      validate ASAM-CMP TX (common_flags bit4=1)                                                         │
│      extract CAN-ID + data  ──► canTransmitWithID()                                                    │
│                                                                                                         │
└─────────────────────────────────────────────────────────────────────────────────────────────────────────┘
                    │ UDP 5010-5020 (ASAM-CMP data frames)          ▲ UDP 5021 (TX inject)
                    ▼                                               │
┌────────────────── MLBevo_Gen2_cmp_gw — cmp_gw process (Linux) ──────────────────────────────────────────┐
│                                                                                                         │
│  CmpReceiver::recv_packet()  ← libpcap / raw socket on iface                                           │
│      │ raw ASAM-CMP UDP bytes                                                                           │
│      ▼                                                                                                  │
│  GwNif::handle_packet()                                                                                 │
│      ├─ payload_type=0x01 (CAN)                                                                        │
│      │     psp_.can_lookup(can_id) → cmp_can_dispatch_entry_t                                          │
│      │     entry->encode(raw_data, dlc, proto_buf, sz) → proto3 wire                                   │
│      │     build GwMessageHeader{bus_type=CAN, can.can_id, can.bus_id, ts_ns, ...}                     │
│      │                                                                                                  │
│      └─ payload_type=0x04 (FlexRay)                                                                    │
│            psp_.flexray_lookup(slot_id, ch) → cmp_dispatch_entry_t                                     │
│            entry->encode(frame_payload+offset, pdu_len, proto_buf, sz) → proto3 wire                   │
│            build GwMessageHeader{bus_type=FLEXRAY, flexray.slot_id, flexray.bus_id, ...}               │
│      │                                                                                                  │
│      ▼ GwMessageHeader (24B) + proto_data                                                              │
│  GwTipcServer::broadcast()  ──────────────────────────────────────────────► client(s)                  │
│                                                                                                         │
│  GwTipcServer::poll()  ◄── TX_REQUEST from client                                                      │
│      psp_.can_lookup(hdr.can.can_id)  → entry->decode(proto_data, len, pdu_out, sz)                    │
│      GwUdpTx::send_can(can_id, iface_id, pdu_out, dlc)  ──► UDP 5021 ──► Hercules                     │
│                                                                                                         │
└─────────────────────────────────────────────────────────────────────────────────────────────────────────┘
                    │ AF_TIPC SOCK_SEQPACKET
                    │ type=0x80010000  instance=0
                    │ TIPC_NODE_SCOPE (single-host loopback)
                    ▼
┌──────────── MLBevo_Gen2_cmp_gw_client — host application ───────────────────────────────────────────────┐
│                                                                                                         │
│  GwClient::recv_signal()                                                                                │
│      recv() → parse GwMessageHeader → identify bus (bus_type + bus_id)                                 │
│      pb_decode(proto_data, proto_len) → application-level signal values                                │
│                                                                                                         │
│  GwClient::send_tx_request(can_id, proto_data, proto_len)                                               │
│      build GwMessageHeader{msg_type=TX_REQUEST, bus_type=CAN, can.can_id, ...}                         │
│      send() over TIPC                                                                                   │
│                                                                                                         │
│  Dependencies: nanopb + generated .pb.h/.pb.c only — no libcmpdecoder                                  │
└─────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Repo Map

| Repo | Branch | Role |
|---|---|---|
| `pero_cmp_ti` | `discovery` | Read-only capture firmware (Hercules TMS570LS3137). Unchanged. |
| `pero_cmp_gw` | `gateway` | **NEW** bidirectional gateway firmware. Sends ASAM-CMP capture frames; receives TX inject frames on UDP 5021. |
| `pero_cmp_lnx` | `gateway` | Linux framework: `cmpdecoder` library, `PspLoader`, `GwNif`/`GwTipcServer`/`GwUdpTx` (in `lib/gw/`), code generators. |
| `MLBevo_Gen2_cmp_psp` | `gateway` | Platform Signal Package: all generated codecs (`libpsp.so`, `libcodec.a`). Includes decode codecs for TX path. |
| `MLBevo_Gen2_cmp_gw` | `gateway` | **NEW** `cmp_gw` executable. Links `libgw.so` from `pero_cmp_lnx`. |
| `MLBevo_Gen2_cmp_gw_client` | `gateway` | **NEW** demo client. Connects via TIPC, decodes signals, can send TX requests. |
| `MLBevo_Gen2_cmp_demo` | `discovery` | Existing FlexRay capture demo. Unchanged. |

---

## 3. Data Flow

### RX Path — vehicle bus → host application

```
Hercules CAN ISR
  └─ populateCmpCanFrame(can_id, data, dlc, iface_id)
       └─ UDP 5010-5020: [CmpHeader 8B][DataMsgHdr 16B][CanPayloadHdr 16B][data dlc]

cmp_gw GwNif::handle_packet(buf, len)
  ├─ validate: buf[0]==0x01 (version), buf[4]==0x01 (data msg)
  ├─ buf[21] == 0x01 (CAN payload):
  │    can_id = be32(buf+28)
  │    data_len = buf[39]
  │    entry = psp_.can_lookup(can_id)          // libpsp.so psp_can_lookup()
  │    proto_len = entry->encode(buf+40, data_len, proto_buf, 256)
  │    hdr.bus_type  = GW_BUS_TYPE_CAN
  │    hdr.can.can_id  = can_id
  │    hdr.can.bus_id  = gw_proto_type_to_bus_id(entry->proto_type)
  │    hdr.can.dlc     = data_len
  │    hdr.timestamp_ns = be64(buf+8)           // ASAM-CMP UTC timestamp
  │    hdr.tipc.sequence_num = seq_num_++
  │    tipc_.broadcast(hdr, proto_buf, proto_len)
  │
  └─ buf[21] == 0x04 (FlexRay payload):
       slot_id = be16(buf+30)
       ch      = (iface_id == 2) ? 1 : 0        // 0=chA 1=chB
       entry   = psp_.flexray_lookup(slot_id, ch)
       proto_len = entry->encode(frame_payload + entry->pdu_byte_offset,
                                 entry->pdu_byte_length, proto_buf, 512)
       hdr.bus_type = GW_BUS_TYPE_FLEXRAY
       hdr.flexray.slot_id = slot_id
       hdr.flexray.bus_id  = gw_proto_type_to_bus_id(entry->proto_type)
       tipc_.broadcast(hdr, proto_buf, proto_len)

GwTipcServer::broadcast()
  └─ for each connected client fd:
       send([GwMessageHeader 24B][proto_data proto_len B])  // SOCK_SEQPACKET

client GwClient::recv_signal()
  └─ recv() → parse GwMessageHeader → pb_decode(proto_data) → signal values
```

### TX Path — host application → vehicle bus

```
client GwClient::send_tx_request(can_id, proto_data, proto_len)
  └─ build GwMessageHeader{msg_type=TX_REQUEST, bus_type=CAN,
                            can.can_id=can_id, proto_len=proto_len}
     send([GwMessageHeader 24B][proto_data proto_len B]) over TIPC

cmp_gw GwTipcServer::poll() receives TX_REQUEST
  └─ GwNif::handle_tx_request(hdr, proto_data, proto_len)
       entry = psp_.can_lookup(hdr.can.can_id)  // same lookup as encode
       n = entry->decode(proto_data, proto_len, pdu_out, 8)  // proto→raw bytes
       iface_id = hdr.can.channel_idx + 1       // 0-based → 1-based
       udp_tx_.send_can(hdr.can.can_id, iface_id, pdu_out, n)

GwUdpTx::send_can()
  └─ build ASAM-CMP frame:
       [CmpHeader 8B: version=1, msg_type=data, seq++]
       [DataMsgHdr 16B: ts=0, iface_id BE32, common_flags=0x10 (TRANSMITTED), payload_type=CAN]
       [CanPayloadHdr 16B: can_id BE32, dlc, data_length]
       [data dlc bytes]
     sendto(sock_, frame, total, 0, &dest_)     // UDP → Hercules port 5021

Hercules gw_tx_recv_callback()
  └─ validate: common_flags bit4==1 (TRANSMITTED)
     extract can_id, iface_id, data
     canTransmitWithID(node, GW_TX_MSGBOX, data, can_id)
```

---

## 4. GwMessageHeader — 24-byte Wire Format

```c
#pragma pack(push, 1)
typedef struct {
    uint8_t       bus_type;       // [0]    GW_BUS_TYPE_CAN=0 | GW_BUS_TYPE_FLEXRAY=1
    uint8_t       msg_type;       // [1]    GW_MSG_SIGNAL_UPDATE=0x01 | GW_MSG_TX_REQUEST=0x02
    uint16_t      proto_len;      // [2-3]  byte count of proto3 wire data that follows
    uint64_t      timestamp_ns;   // [4-11] ASAM-CMP UTC ns timestamp (from Hercules TIME_SYNC)
    union {
        GwCanMeta     can;        // [12-19] when bus_type == GW_BUS_TYPE_CAN
        GwFlexRayMeta flexray;    // [12-19] when bus_type == GW_BUS_TYPE_FLEXRAY
    };
    GwTipcMeta    tipc;           // [20-23]
} GwMessageHeader;  /* 1+1+2+8+8+4 = 24 bytes */
#pragma pack(pop)
```

### GwCanMeta (8 bytes, at offset 12)

| Offset | Field | Width | Description |
|--------|-------|-------|-------------|
| 0 | `can_id` | 4 B | CAN arbitration ID (11- or 29-bit) |
| 4 | `bus_id` | 1 B | `GwBusId` enum value (e.g. `GW_BUS_CAN_KCAN=6`) |
| 5 | `channel_idx` | 1 B | Physical CAN channel 0=CAN1 … 3=CAN4 |
| 6 | `dlc` | 1 B | CAN data length code (0–8) |
| 7 | `flags` | 1 B | bit0=extended 29-bit ID, bit1=RTR |

### GwFlexRayMeta (8 bytes, at offset 12)

| Offset | Field | Width | Description |
|--------|-------|-------|-------------|
| 0 | `slot_id` | 2 B | FlexRay static slot number |
| 2 | `pdu_offset` | 2 B | Byte offset of PDU within frame payload |
| 4 | `cycle` | 1 B | FlexRay cycle counter (0–63) |
| 5 | `channel_idx` | 1 B | 0=channel A, 1=channel B |
| 6 | `bus_id` | 1 B | `GwBusId` enum (e.g. `GW_BUS_MLBEVO_GEN2_A=128`) |
| 7 | `reserved` | 1 B | Future use |

### GwTipcMeta (4 bytes, at offset 20)

| Offset | Field | Width | Description |
|--------|-------|-------|-------------|
| 0 | `sequence_num` | 2 B | Monotonically incrementing per-GW counter for drop detection |
| 2 | `reserved` | 2 B | Future use (priority, client routing, flags) |

The header is followed immediately by `proto_len` bytes of proto3 wire-format data.
`SOCK_SEQPACKET` preserves message boundaries — one `send()` = one complete GwMessage.

---

## 5. GwBusId Enum

`GwBusId` is generated by `gen_gw_types.py` into `gw_bus_types.h`.
Values are stable — they are assigned alphabetically within each class and must not
be renumbered once committed (clients persist them in logs/databases).

```
GW_BUS_INVALID    = 0

// CAN (1–127, alphabetical)
GW_BUS_CAN_DIAGCAN   = 1
GW_BUS_CAN_DCAN      = 2
GW_BUS_CAN_HCAN      = 3
GW_BUS_CAN_ICAN      = 4
GW_BUS_CAN_K2CAN     = 5
GW_BUS_CAN_KCAN      = 6
GW_BUS_CAN_KOMFORTCAN= 7
GW_BUS_CAN_SUBCAN    = 8

// FlexRay clusters (128+; channel A = base, B = base+1)
GW_BUS_MLBEVO_GEN2_A = 128
GW_BUS_MLBEVO_GEN2_B = 129
```

The helper `gw_proto_type_to_bus_id(entry->proto_type)` extracts the namespace
prefix from a proto_type string such as `"can_kcan.ACC_07"` and maps it to the
bus ID. This is called by GwNif at runtime to populate `GwCanMeta.bus_id`.

Client code maps `bus_id` back to a human-readable name via
`gw_namespace_to_bus_id()` or uses it directly as a dictionary key.

---

## 6. PSP Split — libcodec.a vs libpsp.so vs libpsp_local.so

The MLBevo_Gen2_cmp_psp build produces three artifacts with distinct roles:

```
MLBevo_Gen2_cmp_psp/
  build/
    libcodec.a        — static library, ALL encode_*.c + decode_*.c
    libpsp.so         — shared library: libcodec.a + ALL dispatch tables + PSP registry
    libpsp_local.so   — shared library: libcodec.a + app-provided dispatch_local.c
```

### libcodec.a

Contains every generated codec function:
- `cmp_can_encode_<ns>_<MSG>(raw, dlc, out, sz) → size_t`
- `cmp_decode_<ns>_<MSG>(proto_buf, len, pdu_out, sz) → size_t`
- `cmp_encode_<ns>_<PDU>(pdu_bytes, pdu_len, out, sz) → size_t`  (FlexRay)

Static archive with `-fPIC`. The linker discards unreferenced symbols, so
linking only the codecs you need keeps shared library sizes small.

### libpsp.so

Wraps `libcodec.a` plus:
- All `can_dispatch_table.c` files (one per bus)
- `psp_can_registry.c` — `psp_can_lookup(can_id)` cross-bus entry point
- FlexRay `dispatch_table.c`

Used by `cmp_gw` and any tool that needs the complete signal set without
knowing which bus a CAN ID belongs to.

Approximate sizes (all buses + FlexRay, ~1 200 messages):
- `libcodec.a`: ~4–6 MB (all codecs, stripped)
- `libpsp.so`:  ~4–6 MB (same codecs via codec + dispatch overhead)
- `libpsp_local.so` for a 20-signal subset: ~200–400 KB

### libpsp_local.so

For applications that need only a small subset of signals:

```bash
cmake -B build -DLOCAL_DISPATCH_SRC=path/to/my_dispatch_local.c ..
make -C build psp_local
```

`my_dispatch_local.c` declares extern references to only the codecs needed,
builds a `_can_table[]`, and exposes the standard accessor functions.
The linker pulls in only those codec objects from `libcodec.a`.

---

## 7. Deduplication — gen_platform_protos.py

`tools/gen_platform_protos.py` is a unified generator that processes FIBEX and
multiple DBC files in a single pass and detects identical PDU/message layouts
across buses.

### What is a layout fingerprint?

For each CAN message, the fingerprint is:
```python
fingerprint = tuple(sorted(
    (sig.start_bit, sig.bit_length, int(sig.motorola_byte_order),
     sig.factor, sig.offset, sig.name)
    for sig in msg.signals.values()
))
```

Two messages are considered identical if their fingerprints match, regardless
of which bus they appear on. This is common on MLBevo Gen2 where many messages
(e.g. `ACC_07`) are broadcast identically on KCAN and KomfortCAN.

### Deduplication algorithm

1. Parse FIBEX → FlexRay PDU fingerprints.
2. Parse each DBC → CAN message fingerprints.
3. First bus to define a fingerprint: `canonical` owner.
4. Subsequent bus with same fingerprint: mark `is_shared = True`.
5. Shared layouts → single codec in `src/shared/`; proto in `proto/shared/`.
6. Bus dispatch tables reference the shared function via `extern`.

### Output structure

```
src/shared/
    can_encode_ACC_07.c          # size_t cmp_can_encode_ACC_07(...)
    can_decode_ACC_07.c          # size_t cmp_decode_ACC_07(...)

proto/shared/
    ACC_07.proto                 # package shared;

src/can/kcan/
    can_encode_ACC_99.c          # bus-specific (unique layout)
    can_decode_ACC_99.c
    can_dispatch_table.c         # mixes shared + bus-specific externs
    can_dispatch_table.h
    ns_wrapper.h
    psp_manifest.json

src/can/komfortcan/
    can_dispatch_table.c         # ACC_07: extern cmp_can_encode_ACC_07 (shared)

src/flexray/
    encode_ACC_06.c
    decode_ACC_06.c
    dispatch_table.c
    dispatch_table.h
    hercules_filter.h

src/
    psp_can_registry.c           # psp_can_lookup() across all buses
    psp_can_registry.h
```

### Dispatch table .c when a message is shared

```c
/* src/can/kcan/can_dispatch_table.c  (generated) */
extern size_t cmp_can_encode_can_kcan_ACC_99(const uint8_t*, size_t, uint8_t*, size_t);
extern size_t cmp_decode_can_kcan_ACC_99(const uint8_t*, size_t, uint8_t*, size_t);

/* Shared codec declarations */
extern size_t cmp_can_encode_ACC_07(const uint8_t*, size_t, uint8_t*, size_t);  /* shared */
extern size_t cmp_decode_ACC_07(const uint8_t*, size_t, uint8_t*, size_t);     /* shared */

static const cmp_can_dispatch_entry_t _can_table[] = {
    { 0x12Eu, 0u, "ACC_07", "shared.ACC_07",
      cmp_can_encode_ACC_07, cmp_decode_ACC_07, 8u, 80u },   /* shared codec */
    { 0x1A3u, 0u, "ACC_99", "can_kcan.ACC_99",
      cmp_can_encode_can_kcan_ACC_99, cmp_decode_can_kcan_ACC_99, 8u, 96u },
};
```

Note the `proto_type` field: `"shared.ACC_07"` means the namespace used in the
`.proto` file is `shared`, so the client decodes with the `shared.ACC_07` proto
message definition.

---

## 8. libgw — GW Library

The GW library lives in `pero_cmp_lnx/lib/gw/` and is built as `libgw.so` by
the `pero_cmp_lnx` CMake target `gw`.

### Files

```
pero_cmp_lnx/lib/gw/
    include/
        gw_proto.h        — GwMessageHeader, GwCanMeta, GwFlexRayMeta, GwTipcMeta
        gw_bus_types.h    — GwBusId enum, gw_proto_type_to_bus_id()
        gw_nif.h          — GwNif class declaration
        gw_tipc_server.h  — GwTipcServer class declaration
        gw_udp_tx.h       — GwUdpTx class declaration
    src/
        gw_nif.cpp        — main packet processing loop
        gw_tipc_server.cpp— AF_TIPC SOCK_SEQPACKET + epoll fan-out
        gw_udp_tx.cpp     — ASAM-CMP TX frame builder + sendto
```

### CMake target

Defined in `pero_cmp_lnx/lib/CMakeLists.txt`:

```cmake
add_library(gw SHARED
    gw/src/gw_nif.cpp
    gw/src/gw_tipc_server.cpp
    gw/src/gw_udp_tx.cpp
)
target_include_directories(gw
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/gw/include   # gw_proto.h, gw_nif.h, ...
        ${CMAKE_CURRENT_SOURCE_DIR}/include       # cmp_plugin.h, psp_loader.h
    PRIVATE
        ${EXPAT_INCLUDE_DIRS}
)
target_link_libraries(gw
    PUBLIC  cmpdecoder       # PspLoader, CmpReceiver
    PRIVATE ${EXPAT_LIBRARIES} ${PCAP_LIB}
)
```

### Linking the GW executable

`MLBevo_Gen2_cmp_gw/CMakeLists.txt` finds and links `libgw.so`:

```cmake
find_library(GW_LIB  gw         HINTS ${PERO_CMP_ROOT}/build/lib)
find_library(CMP_LIB cmpdecoder HINTS ${PERO_CMP_ROOT}/build/lib)
find_library(PSP_LIB psp        HINTS ${PSP_ROOT}/build)

target_include_directories(cmp_gw PRIVATE
    ${PERO_CMP_ROOT}/lib/gw/include   # GwNif, GwMessageHeader
    ${PERO_CMP_ROOT}/lib/include      # PspLoader, cmp_plugin.h
    ${PSP_ROOT}/src                   # psp_can_registry.h
)
target_link_libraries(cmp_gw PRIVATE ${GW_LIB} ${CMP_LIB} ${PSP_LIB} ...)
```

---

## 9. Build Workflow

### Prerequisites

```bash
sudo apt install libexpat1-dev libpcap-dev nanopb
python3 -m pip install jinja2
modprobe tipc          # TIPC kernel module for TIPC sockets
```

### Step 1 — Generate PSP source files

```bash
cd MLBevo_Gen2_cmp_psp
./generate.sh
# Produces: src/flexray/*, src/can/*/, proto/*/
```

Or use the unified generator (with deduplication):

```bash
cd MLBevo_Gen2_cmp_psp

TOOLS=../pero_cmp_lnx/tools
FIBEX=config/MLBevo_Gen2_Fx_Cluster_KMatrix_V8.17.02F_20181219_SEn_DefaultValuePatch.xml

python3 $TOOLS/gen_platform_protos.py \
    --fibex "$FIBEX" \
    --dbc config/dbc/MLBevo_Gen2_MLBevo_KCAN_KMatrix_V8.27.01F.dbc:kcan \
    --dbc config/dbc/MLBevo_Gen2_MLBevo_HCAN_KMatrix_V8.27.01F.dbc:hcan \
    --dbc config/dbc/MLBevo_Gen2_MLBevo_ICAN_KMatrix_V8.27.01F.dbc:ican \
    --dbc config/dbc/MLBevo_Gen2_MLBevo_DCAN_KMatrix_V8.20.03F_20210316_EICR.dbc:dcan \
    --dbc config/dbc/CAN_2_MLBevo_Gen2_MLBevo_KomfortCAN_KMatrix_V8.15.00F_20171109_FB.dbc:komfortcan \
    --dbc config/dbc/MLBevo_Gen2_MLBevo_K2CAN_KMatrix_V8.27.01F.dbc:k2can \
    --dbc config/dbc/MLBevo_Batterie_SUBCAN_KMatrix_V8.16.00F_20180608_TR.dbc:subcan \
    --dbc config/dbc/MLBevo_Gen2_MLBevo_DiagCAN_KMatrix_V8.20.00F_20200528_AH.dbc:diagcan \
    --namespace-fr mlbevo_gen2 \
    --all-signals \
    --out-src src/ --out-proto proto/
```

### Step 2 — Build libpsp.so

```bash
cd MLBevo_Gen2_cmp_psp
cmake -B build -DPERO_CMP_INCLUDE=../pero_cmp_lnx/lib/include
make -C build psp
# → build/libpsp.so  build/libcodec.a
```

Verify decode symbols are present:

```bash
nm -D build/libpsp.so | grep cmp_decode | head -10
```

### Step 3 — Build pero_cmp_lnx (libgw.so + libcmpdecoder.a)

```bash
cd pero_cmp_lnx
cmake -B build
make -C build gw cmpdecoder
# → build/lib/libgw.so  build/lib/libcmpdecoder.a
```

### Step 4 — Build cmp_gw

```bash
cd MLBevo_Gen2_cmp_gw
cmake -B build \
    -DPERO_CMP_ROOT=../pero_cmp_lnx \
    -DPSP_ROOT=../MLBevo_Gen2_cmp_psp
make -C build
# → build/cmp_gw
```

### Step 5 — Build cmp_gw_client

```bash
cd MLBevo_Gen2_cmp_gw_client
cmake -B build
make -C build
# → build/cmp_gw_client
```

### Rebuilding after DBC/FIBEX changes

Only Steps 1–2 need re-running when signal definitions change.
Steps 3–4 only need re-running when pero_cmp_lnx headers or GW sources change.

---

## 10. Testing

### Basic connectivity test

Run on the same Linux host (TIPC loopback):

```bash
# Terminal 1 — GW (replace eth0 with capture interface, 192.168.1.100 with Hercules IP)
sudo build/cmp_gw eth0 192.168.1.100 ../MLBevo_Gen2_cmp_psp/build

# Terminal 2 — Client
./build/cmp_gw_client
```

Expected output on GW start:
```
[GwTipcServer] Listening on TIPC {0x80010000, 0}
[GwUdpTx] Ready to send to 192.168.1.100:5021
GW running. Ctrl-C to stop.
[GwTipcServer] Client connected (fd=5), total=1
```

### Using pero-timesync for accurate timestamps

`pero_cmp_lnx` provides a timesync pusher that sends TIME_SYNC ASAM-CMP frames
to Hercules, synchronising the Hercules 64-bit timestamp to host UTC:

```bash
# pero-timesync demo (from pero_cmp_lnx/demo/timesync/)
./build/demo/timesync/pero_timesync eth0 192.168.1.100
```

Once synced, `GwMessageHeader.timestamp_ns` reflects UTC nanoseconds from the
Hercules capture hardware clock.

### Using pero-filter to limit captured messages

The `pero-filter` demo sends a Hercules control message (UDP 5001) to enable
only specific CAN IDs and FlexRay slot IDs:

```bash
# Send filter to capture only KCAN 0x12E and FlexRay slot 4
./build/demo/filter/pero_filter eth0 192.168.1.100 \
    --can 0x12E --slot 4
```

This reduces UDP traffic from Hercules and TIPC message rate in the GW.

### Full integration test sequence

1. Flash `pero_cmp_gw` firmware to Hercules.
2. Connect Hercules Ethernet to Linux host (or same LAN segment).
3. `modprobe tipc` on Linux host.
4. Run `pero_timesync` — wait for `TIME_SYNC ACK` (typically < 2 s).
5. Run `pero_filter` — set desired CAN IDs and FlexRay slots.
6. Run `cmp_gw eth0 <hercules-ip> <psp-root>`.
7. Run `cmp_gw_client` — verify SIGNAL_UPDATE messages received.
8. Send a TX_REQUEST from the client — verify on a CAN analyser that the
   frame appears on the bus.

### Verifying TX injection

On Hercules, `gw_tx_recv_callback()` is wired to UDP port 5021.
Use Wireshark on the host with capture filter `udp.dstport == 5021` to
confirm the ASAM-CMP TX frame is sent by `GwUdpTx::send_can()`.
On the CAN bus side, use a CAN interface (`ip link set canX up type can bitrate 500000`)
and `candump canX` to observe the injected frame.

### Diagnostic checks

```bash
# Verify libpsp.so exports
nm -D MLBevo_Gen2_cmp_psp/build/libpsp.so | grep psp_can

# Verify libgw.so exports
nm -D pero_cmp_lnx/build/lib/libgw.so | grep GwNif

# Check TIPC module
lsmod | grep tipc

# List TIPC services (after cmp_gw starts)
tipc nametable show 2>/dev/null || ip tipc nametable show

# strace GW socket calls
strace -e trace=socket,bind,listen,accept,send,recv,sendto,recvfrom \
    -p $(pgrep cmp_gw)
```

---

## 11. TIPC Addressing

```c
#define TIPC_GW_TYPE      0x80010000U   // GW service type
#define TIPC_GW_INSTANCE  0U            // GW instance number
// Scope: TIPC_NODE_SCOPE (single-host; change to TIPC_CLUSTER_SCOPE for multi-host)
```

The GW binds `AF_TIPC SOCK_SEQPACKET` to this service address.
Clients connect to the same `{type, instance}` — TIPC routes to the bound server.

`epoll` on the GW side monitors:
- `listen_fd_`: new client connections (`accept()`)
- Each client fd: `EPOLLIN` for TX_REQUEST, `EPOLLRDHUP|EPOLLERR` for disconnect

The main loop interleaves CMP packet reception (50 ms `recv_packet()` timeout)
with TIPC polling (0 ms = non-blocking). This single-threaded design avoids
mutex overhead and is sufficient for gateway demo data rates.

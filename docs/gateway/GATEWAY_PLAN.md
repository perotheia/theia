# CAN/FlexRay ↔ Linux Gateway — Architecture & Implementation Plan

## 1. Architecture (ASCII)

```
┌──────────────────────────────────────── pero_cmp_gw (Hercules) ──────────────────────────────────────────┐
│                                                                                                           │
│  CAN1-4 ──ISR──► onCanDataReceived() ──► populateCmpCanFrame() ──► cmp_can_id_is_accepted() ──► UDP TX  │
│  FlexRay ──poll──► receiveFlexrayAndSendUdp() ──► populateCmpFlexRayFrame() ──► UDP TX                   │
│                                                                                                           │
│  UDP port 5021 ◄── gw_tx_recv_callback()  NEW                                                            │
│      parse ASAM-CMP TX (common_flags bit4=1) → extract CAN ID+data → canTransmitWithID()                │
│                                                                                                           │
│  UDP 5010-5020 out (capture)    UDP 5021 in (injection)    UDP 5001 (filter/timesync/status)             │
└───────────────────────────────────────────────────────────────────────────────────────────────────────────┘
                                │UDP                            ▲UDP
                                ▼                               │
┌─────────────────────── MLBevo_Gen2_cmp_gw (Linux NIF) ────────────────────────────────────────────────────┐
│                                                                                                           │
│  CmpReceiver (UDP 5010-5020)                                                                             │
│      │ raw ASAM-CMP bytes                                                                                 │
│      ▼                                                                                                    │
│  parse CAN/FR payload → psp_can_lookup(can_id) / psp.flexray_lookup(slot,ch)                            │
│      │                → entry->encode(data, len, proto_buf, sz)                                          │
│      ▼ proto3 wire + GwMessageHeader                                                                     │
│  GwTipcServer::broadcast() ─────────────────────────────────────────────────────────► GW_client         │
│                                                                                                           │
│  GwTipcServer::poll() ◄──────────────────────────────────────────────────────── GW_client TX_REQUEST    │
│      │ GwMessage{TX_REQUEST, can_id, proto_data}                                                         │
│      ▼                                                                                                    │
│  psp_can_decode_lookup(can_id) → entry->decode(proto_buf, len, pdu_out, sz)  NEW                        │
│      │ raw PDU bytes                                                                                      │
│      ▼                                                                                                    │
│  GwUdpTx::send_can() → ASAM-CMP frame (common_flags bit4=1) → UDP 5021 → Hercules                      │
└───────────────────────────────────────────────────────────────────────────────────────────────────────────┘
                    │ TIPC AF_TIPC SOCK_SEQPACKET (type=0x80010000, inst=0)
                    ▼
┌──────── MLBevo_Gen2_cmp_gw_client ─────────────────────────────────────────────────────────────────────────┐
│  GwClient::recv_signal() → pb_decode() → application signal values                                       │
│  GwClient::send_tx_request(can_id, proto_data) → GwMessage{TX_REQUEST} → TIPC send                      │
│  Links: nanopb only — no libcmpdecoder dependency                                                        │
└───────────────────────────────────────────────────────────────────────────────────────────────────────────┘

PSP layer — libpsp.so (MLBevo_Gen2_cmp_psp, gateway branch extended):
  encode (existing): cmp_can_encode_<ns>_<MSG>(raw_bytes → proto_wire)
  decode (NEW):      cmp_decode_<ns>_<MSG>(proto_wire → raw_bytes)   [pdu_decode.c.j2]
  lookup (existing): psp_can_lookup(can_id)
  decode lookup(NEW):psp_can_decode_lookup(can_id)  [same entry, decode fn ptr]
```

---

## 2. Repo Map

| Repo | Branch | Role |
|---|---|---|
| `pero_cmp_ti` | `discovery` | Existing capture-only firmware (unchanged) |
| `pero_cmp_gw` | `gateway` | **NEW** — bidirectional gateway firmware (CLEANUP + gw_tx) |
| `pero_cmp_lnx` | `gateway` | Framework: additive headers + generator extensions |
| `MLBevo_Gen2_cmp_psp` | `gateway` | Extended PSP: adds decode codec to libpsp.so |
| `MLBevo_Gen2_cmp_demo` | `discovery` | Existing demo (unchanged) |
| `MLBevo_Gen2_cmp_gw` | `gateway` | **NEW** — Linux NIF (TIPC server + UDP TX) |
| `MLBevo_Gen2_cmp_gw_client` | `gateway` | **NEW** — host app (TIPC client, pb_decode, TX) |

---

## 3. TIPC Protocol

### Addressing (hardcoded for demo)
```c
#define TIPC_GW_TYPE     0x80010000U   // GW service type
#define TIPC_GW_INSTANCE 0U            // GW instance number
// Both GW and client run on same host (single-node TIPC via loopback)
// TIPC_NODE_SCOPE → change to TIPC_CLUSTER_SCOPE for multi-host later
```

### GwMessage Wire Format (12-byte header + proto_data, little-endian)
```c
#pragma pack(push, 1)
struct GwMessageHeader {
    uint32_t msg_type;    // 0x01=SIGNAL_UPDATE (GW→client)  0x02=TX_REQUEST (client→GW)
    uint32_t can_id;      // CAN arbitration ID (11 or 29 bit)
                          // FlexRay: slot_id in [15:0], bit31=1
    uint8_t  channel_idx; // 0=CAN1 1=CAN2 2=CAN3 3=CAN4  0x10=FR-chA 0x11=FR-chB
    uint8_t  bus_type;    // 0=CAN  1=FlexRay
    uint16_t proto_len;   // byte count of proto_data that follows
    // uint8_t proto_data[proto_len]  — nanopb proto3 wire, immediately after header
};
#pragma pack(pop)
```

`SOCK_SEQPACKET` preserves message boundaries — one send = one GwMessage.
Server uses `epoll` across: TIPC listen fd + N client fds + CmpReceiver UDP sockets.

---

## 4. Hercules TX Path (`pero_cmp_gw` new file: `gw_tx.c/h`)

```
Host → UDP port 5021 → gw_tx_recv_callback()
    Validate: cmp_version==0x01, msg_type==0x01
    Check:    DataMessageHeader.common_flags bit 4 == 1  (TRANSMITTED)
    Check:    payload_type == 0x01 (CAN)
    Extract:  interface_id → canREG1/2/3/4
              can_id (bytes [28-31], big-endian)
              data   (bytes [40+], data_length bytes)
    Call:     canTransmitWithID(node, GW_TX_MSGBOX, data, can_id)
```

```c
#define GW_TX_PORT    5021U   // UDP port for host→Hercules ASAM-CMP TX
#define GW_TX_MSGBOX     2U   // CAN TX message box (confirm free in HL_can.c)
void gw_tx_init(const ip_addr_t* src_ip);  // call after cmp_control_init()
```

**FlexRay TX**: deferred — requires pre-allocated TX slots in the schedule.

---

## 5. PSP TX Codec (proto wire → PDU bytes)

### New headers in `pero_cmp_lnx/lib/include/`

- `cmp_wire_reader.h` — header-only proto wire parser (`cmp_reader_t`, `cmp_read_field()`)
- `cmp_write_bits.h` — header-only bit writer (`cmp_write_bits_intel()`, `cmp_write_bits_motorola()`)

### `cmp_plugin.h` change (ABI break — all binaries must recompile)

```c
typedef size_t (*cmp_decode_fn_t)(const uint8_t* proto_buf, size_t proto_len,
                                   uint8_t* pdu_out,        size_t pdu_size);
// Add to both cmp_dispatch_entry_t and cmp_can_dispatch_entry_t:
    cmp_decode_fn_t decode;   // proto wire → PDU bytes; NULL for encode-only
```

### New generator output: `can_decode_<MSG>.c`

Template `pdu_decode.c.j2` — for each field:
- wire_type 5 (float): `raw = round((physical - offset) / scale)` → `cmp_write_bits_intel/motorola()`
- wire_type 0 (varint): raw → `cmp_write_bits_*()`
- Signed: `int32_t raw_s = round((float - offset) / scale)` → cast to uint64

### New PSP registry function

```c
// psp_can_registry.h / .c (generated)
const cmp_can_dispatch_entry_t* psp_can_decode_lookup(uint32_t can_id);
// Identical to psp_can_lookup() — returns same entry; decode fn ptr accessed via entry->decode
```

---

## 6. Per-Repo Changes

### pero_cmp_gw/ (new repo)
**Remove** (per CLEANUP.md): `can_message.pb.*`, `custom_serialization.*`, `message_cache_serialization.*`, `msg_pack_serialization.*`, `pb_*.c/h`, `msgpack/`, `raw_frame.h`  
**Modify** `HL_sys_main.c`: remove dead includes/calls/buffers; add `gw_tx_init(&src_ip)`  
**Modify** `FlexRay.c`: remove dead include and `populateRawFrame()`  
**Add** `include/gw_tx.h` + `source/gw_tx.c`  
**Update** `.cproject`: remove deleted files, add `gw_tx.c`

### pero_cmp_lnx/ (gateway branch, additive only)
**Add** `lib/include/cmp_wire_reader.h`, `lib/include/cmp_write_bits.h`  
**Modify** `lib/include/cmp_plugin.h`: add `cmp_decode_fn_t` + `decode` field  
**Add** `tools/templates/pdu_decode.c.j2` (CAN), `tools/templates/pdu_fr_decode.c.j2` (FlexRay)  
**Modify** `tools/can_to_nanopb.py`: render decode template per message  
**Modify** `tools/fibex_to_nanopb.py`: render FlexRay decode template  
**Modify** dispatch table + registry templates: extern + populate decode field; add `psp_can_decode_lookup`

### MLBevo_Gen2_cmp_psp/ (gateway branch)
Re-run `generate.sh` → new `can_decode_*.c` files alongside existing `can_encode_*.c`  
Rebuild `libpsp.so` → verify `nm build/libpsp.so | grep cmp_decode`

### MLBevo_Gen2_cmp_gw/ (new repo)
Scaffold from `MLBevo_Gen2_cmp_demo/`  
Add `gw_tipc_server.h/.cpp`, `gw_udp_tx.h/.cpp`, `gw_nif.h/.cpp`  
Rewrite `main.cpp` as GW process

### MLBevo_Gen2_cmp_gw_client/ (new repo)
Standalone: `gw_proto.h` (struct defs), `gw_client.h/.cpp`, `main.cpp`  
Links: nanopb + generated `.pb.h/.pb.c` only — no libcmpdecoder

---

## 7. Implementation Phases

| Phase | Scope | Depends on | Priority |
|---|---|---|---|
| 1 | PSP TX codec (wire_reader, write_bits, pdu_decode template, generator extension) | — | Highest |
| 2 | Firmware cleanup (pero_cmp_gw — dead code removal per CLEANUP.md) | — | High |
| 3 | Firmware GW TX (gw_tx.c, UDP port 5021, canTransmitWithID) | Phase 2 | High |
| 4 | Linux GW NIF (cmp_gw — TIPC server + UDP TX path) | Phase 1 | High |
| 5 | GW client (cmp_gw_client — TIPC client + pb_decode + TX demo) | Phase 4 | Medium |
| 6 | Integration test + regression | All | Final |

---

## 8. Open Questions

1. **GW_TX_MSGBOX**: Which CAN message box index is free for TX injection? Check `HL_can.c` generated code for the last configured RX box on each controller.
2. **TIPC kernel module**: Verify `tipc.ko` is available (`modprobe tipc`; `lsmod | grep tipc`).
3. **ABI break**: Recompile ALL binaries (cmp_demo, test_plugin, pero-decode) against updated `cmp_plugin.h` after adding `decode` field.
4. **FlexRay TX**: Deferred — requires pre-reserved TX slots in the FlexRay schedule (FIBEX cluster config). Lower priority for gateway demo.
5. **ASAM-CMP direction field**: TX frames use `DataMessageHeader.common_flags bit4=1` (per spec) — different from `AsamCmpCan_T.flags bit0` (CAN frame direction). Firmware must check the right field.
6. **Thread safety in GW**: Single-threaded `epoll` loop avoids mutex overhead. Sufficient for demo rates.

---

## 9. What Does NOT Change

- `pero_cmp_ti/` (discovery) — untouched; `pero_cmp_gw/` is a separate new repo
- `MLBevo_Gen2_cmp_demo/` (discovery) — unchanged
- ASAM-CMP capture ports 5010-5020 — additive; port 5021 is new
- Control protocol port 5001 — unchanged
- Existing encode functions in libpsp.so — decode functions added alongside, no changes to encode

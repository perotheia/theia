# Signal Pipeline — ISO Layer Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│  HERCULES TMS570 (pero_cmp_ti)           LINUX HOST (pero_cmp_lnx)      │
│                                                                          │
│  L1  FlexRay bus 10 Mbit/s                                              │
│      ─────────────────────────────────────────────────────              │
│  L2  DCAN/FRAY HAL  HL_can.h/Fr.h                                       │
│      FIFO polling (FlexRay)                                             │
│      Interrupt-driven (CAN)                                             │
│      ─────────────────────────────────────────────────────              │
│  L3  Slot filter  cmp_control.c                                         │
│      128-bit bitmap, port 5001                                          │
│      ─────────────────────────────────────────────────────              │
│  L4  ASAM-CMP framing                    ASAM-CMP receive               │
│      AsamCmpFlexRay.c                    libcmpdecoder.a                │
│      AsamCmpCan.c                        CmpReceiver / CmpPcapReader    │
│      lwIP UDP 5010-5013, 5020            decode_flexray_packet()        │
│      ─────────────────────────────────── ────────────────────────────── │
│                                                                          │
│                     UDP/Ethernet  (169.254.8.x)                         │
│                                                                          │
│                                          ─────────────────────────────── │
│                                    L6    FlexRay → proto3 wire           │
│                                          libcmp_mlbevo_v8_17.so          │
│                                          cmp_encode_ACC_06()             │
│                                          cmp_encode_STS_01()  …          │
│                                          cmp_get_dispatch_table()        │
│                                          ─────────────────────────────── │
│                                    L7    Application                     │
│                                          pb_decode() → POD struct       │
│                                          mlbevo_v8_17::ACC_06           │
│                                          app_dispatch + callbacks        │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## Layer definitions

### L1 — Physical (FlexRay / CAN bus)

| Item | Detail |
|---|---|
| Protocol | FlexRay 2.1, 10 Mbit/s, dual channel A/B |
| Cluster | MLBevo_Gen2, 64-cycle schedule, 51 static slots |
| HW | Hercules TMS570LC43xx on-chip FRAY controller (FRAY1) |
| Files | `FlexRay.c`, `Fr.c`, `flexray_parameter_def.h` — timing constants |

CAN: 4 × DCAN nodes (canREG1–4), 500 kbps, interrupt-driven.

---

### L2 — Data link (HAL)

**Hercules side** — HAL-generated, do not modify:
- `HL_can.h/c` — `canGetData()`, `getDataLength()`, `isMessageExtended()`
- `Fr.h/c` — `Fr_ReceiveRxLPdu()`, RDHS1/2/3 registers for slot_id, cycle, payload

**Linux side** — Ethernet frame:
- `CmpPcapReader` strips Eth(14) + IPv4(IHL×4) + UDP(8) → UDP payload
- `CmpReceiver` uses `AF_INET/SOCK_DGRAM` bound to each CMP port

---

### L3 — Network (slot filter / UDP routing)

**Hercules — dynamic slot filter** (`include/cmp_control.h`, `source/cmp_control.c`):

| Message | Byte 0 | Payload | Description |
|---|---|---|---|
| `FILTER_SET` | `0x01` | `[count:u16LE][slot:u16LE×N]` | Accept only listed slots |
| `FILTER_CLEAR` | `0x02` | — | Accept all slots |
| `STATUS_REQ` | `0x03` | — | Query active filter |
| `STATUS_RSP` | `0x04` | `[active:u16LE][sent:u32LE][dropped:u32LE]` | Response |

Implemented as lwIP `udp_recv` callback on port **5001**.  
Internal state: `volatile uint64_t slot_filter_bitmap[2]` (slots 0–127).

**Linux — send filter from app** (`lib/include/hercules_control.h`):
```cpp
HerculesControl ctrl;
ctrl.open("169.254.8.3");
ctrl.send_filter(mlbevo_v8_17::kHerculesFilterSlots,
                 mlbevo_v8_17::kHerculesFilterSlotCount);
```
Filter slot constants are generated into `app_example/protos/hercules_filter.h`
from `adas_support_functions.csv` — the source of truth.

**UDP port assignments** (HERCULES=3):

| Source | Port | Channel |
|---|---|---|
| FlexRay | 5020 | FRAY1 |
| CAN1 | 5010 | canREG1 — PCB transceiver |
| CAN2 | 5011 | canREG2 — PCB transceiver |
| CAN3 | 5012 | canREG3 — extender board |
| CAN4 | 5013 | canREG4 — extender board |

---

### L4/5 — Transport (ASAM-CMP)

**Wire format** — all multi-byte fields **big-endian** (ASAM-CMP standard):

```
Offset  Size  Field
──────  ────  ────────────────────────────────────────────
  0      1    cmp_version   (0x01)
  1      1    reserved
  2      2    device_id     (= HERCULES define: 1/2/3)
  4      1    message_type  (0x01 = data)
  5      1    stream_id     (0=FlexRay, 1-4=CAN1-4)
  6      2    stream_sequence_counter
──── DataMessageHeader (16 bytes) ────
  8      8    timestamp_ns  (RTI, 1 ms resolution → ns)
 16      4    interface_id  (FlexRay: 0=ch.A, 1=ch.B; CAN: 1-4)
 20      1    common_flags
 21      1    payload_type  (0x01=CAN, 0x04=FlexRay)
 22      2    payload_length
──── FlexRay payload (14 + data_len bytes) ────
 24      2    flags
 26      2    reserved_0
 28      2    header_crc
 30      2    frame_id      ← FIBEX slot_id
 32      1    cycle         (0-63)
 33      3    frame_crc     (not captured by HAL)
 36      1    reserved_1
 37      1    data_length
 38      N    frame payload (up to 254 bytes)
```

**Linux library**: `libcmpdecoder.a`  
Key function: `decode_flexray_packet(udp_payload, len, FibexDb&) → optional<DecodedFrame>`  
Extracts: slot_id, cycle, channel_idx, timestamp_ns, raw frame payload bytes.

---

### L6 — Representation (FlexRay PDU → proto3 wire)

**Generated per FIBEX × CSV** → compiled into `libcmp_<namespace>.so`

The representation layer re-encodes the FlexRay bit-packed PDU bytes into
**proto3 wire format** using bit extraction logic and scale/offset from FIBEX.
No schema knowledge required on the encode side — field numbers are baked
into the generated encoder at generation time.

**Proto3 wire encoding** (`lib/include/cmp_proto_wire.h`):

| Signal type | Proto field type | Wire type |
|---|---|---|
| UNSIGNED, bit_len=1 | `bool` | varint (0/1) |
| UNSIGNED, bit_len≤32, scale=1,offset=0 | `uint32` | varint |
| SIGNED, bit_len≤32, scale=1,offset=0 | `int32` | varint (sign-extended) |
| Any with scale≠1 or offset≠0 | `float` (physical) | 32-bit LE |

**Bit extraction** (`lib/include/cmp_extract.h`):
- Intel (`IS-HIGH-LOW-BYTE-ORDER=false`): `bit_pos` = LSB
- Motorola (`IS-HIGH-LOW-BYTE-ORDER=true`): `bit_pos` = MSB (FIBEX convention)

**Build-time dispatch table** — accessed via functions (no dlsym):
```c
const cmp_dispatch_entry_t* cmp_get_dispatch_table(void);
size_t                       cmp_get_dispatch_count(void);
const char*                  cmp_get_fibex_version(void);
```

**Lookup**:
```c
const cmp_dispatch_entry_t* e = cmp_lookup(slot_id, channel_idx);
size_t n = e->encode(pdu_bytes, pdu_len, proto_buf, sizeof(proto_buf));
```

---

### L7 — Application (nanopb POD structs)

**Generated per FIBEX × CSV** → committed into `app_example/protos/`

```
app_example/
├── CMakeLists.txt          — links L4/5 + L6 + nanopb; lists PROTO_SRCS
├── main.cpp                — receive → dispatch → pb_decode → app logic
└── protos/                 — COMMITTED generated artifacts (interface contract)
    ├── dispatch_table.h    — cmp_get_dispatch_table() declarations + cmp_lookup()
    ├── hercules_filter.h   — constexpr slot list for HerculesControl::send_filter()
    ├── ns_wrapper.h        — C++ namespace typedefs  namespace mlbevo_v8_17 { ... }
    ├── ACC_06.pb.h/.pb.c   — nanopb POD:  mlbevo_v8_17_ACC_06_t
    ├── ACC_17.pb.h/.pb.c   —              mlbevo_v8_17_ACC_17_t
    ├── STS_01.pb.h/.pb.c   —              mlbevo_v8_17_STS_01_t
    └── Waehlhebel_04.pb.h/.pb.c          mlbevo_v8_17_Waehlhebel_04_t
```

**C++ typedef idiom** (from `ns_wrapper.h`):
```cpp
namespace mlbevo_v8_17 {
    using ACC_06 = ::mlbevo_v8_17_ACC_06;  // POD, no vtable, no alloc
}
```

**Decode**:
```cpp
mlbevo_v8_17::ACC_06 msg = mlbevo_v8_17_ACC_06_init_zero;
pb_istream_t stream = pb_istream_from_buffer(proto_buf, proto_len);
pb_decode(&stream, mlbevo_v8_17_ACC_06_fields, &msg);
// → msg.ACC_06_CRC, msg.ACC_06_BZ  — plain C fields
```

**Multiple FIBEX versions** coexist without symbol collision because nanopb
uses the proto `package` as a C identifier prefix (`mlbevo_v8_17_`, `mlbevo_v8_18_`).

---

## Generator workflow

```
                        adas_support_functions.csv
                               +
                          FIBEX XML
                               │
              python3 generator/fibex_to_nanopb.py
                --fibex  ...xml
                --csv    ...csv
                --ns     mlbevo_v8_17
                --out    generated/mlbevo_v8_17/
                               │
               ┌───────────────┼──────────────────────┐
               │               │                      │
         *.proto          hercules_filter.json     encode_*.c
         (schema)          (Python sender /         dispatch_table.h/c
               │            HerculesControl)        ns_wrapper.h
               │
   protoc --nanopb_out=generated/mlbevo_v8_17/
               │
          *.pb.h / *.pb.c
               │
    copy into app_example/protos/  ← commit these
```

To update signals: edit CSV → re-run generator → re-run protoc → copy → rebuild.

---

## Runtime data flow (per FlexRay frame)

```
Hercules FRAY registers
    │  slot_id=4, cycle=12, data[34]
    ↓
AsamCmpFlexRay.c
    │  wrap in CMP header + DataMsgHeader
    │  udp_send() via lwIP
    ↓
────────────── UDP/Ethernet ──────────────
    ↓
CmpReceiver::recv_packet()          [L4]
    │  raw UDP payload bytes
    ↓
cmp_lookup(slot_id=4, ch=0)         [L5]
    │  → &dispatch_entry{ACC_06, offset=24, len=8}
    ↓
cmp_encode_ACC_06(pdu, 8, buf, 256) [L6]
    │  extract bits: CRC[7:0]=0xAB, BZ[11:8]=3
    │  proto wire: [0x08 0xAB 0x01 0x10 0x03]
    ↓
pb_decode() → ACC_06{CRC=171, BZ=3} [L7]
    ↓
on_ACC_06(msg) — application logic
```

# TIMESYNC — Linux host (pero_cmp_lnx)

## Responsibility

The Linux host owns the accurate clock. It periodically pushes the current
UTC time to the Hercules so ASAM-CMP frame timestamps become absolute and
GPS-traceable. The host does **not** consume time from the Hercules.

---

## Clock source

Priority order in `hercules_control.cpp`:

| Clock | `clock_gettime` id | When used |
|---|---|---|
| `CLOCK_TAI` | International Atomic Time | Available on GPS-disciplined hosts (gpsd + PPS); no leap-second jumps |
| `CLOCK_REALTIME` | UTC (NTP/PTP adjusted) | Fallback when `CLOCK_TAI` fails |

`CLOCK_TAI` is preferred because it has no leap-second discontinuities —
a 1-second backward jump in `CLOCK_REALTIME` would corrupt the Hercules
anchor mid-capture.

### GPS lock detection

`adjtimex()` is called before each push to read the kernel clock discipline
status bits:

```cpp
struct timex tx {};
adjtimex(&tx);
uint8_t flags = 0;
if (tx.status & STA_PPSSIGNAL) flags |= 0x01;  // GPS_LOCKED
if (tx.status & STA_NANO)      flags |= 0x04;  // nanosecond resolution
```

`STA_PPSSIGNAL` is set by the kernel when a 1-PPS signal from a GPS receiver
is being tracked (requires `gpsd` + PPS-capable serial device or dedicated
GPS HAT).

---

## Wire format (port 5001, little-endian)

```
Byte  Len  Field
   0    1  Opcode = 0x05
   1    1  flags
              bit 0  GPS_LOCKED
              bit 2  NANO_RES
   2    8  utc_ns  u64 LE — UTC ns since Unix epoch (from CLOCK_TAI or CLOCK_REALTIME)
```

The timestamp is sampled **immediately before** `sendto()` to minimise the
transmit offset. No round-trip compensation is applied; the typical one-way
delay on a direct Gigabit link is < 0.1 ms.

---

## HerculesControl::push_time_sync()

`lib/include/hercules_control.h` / `lib/src/hercules_control.cpp`

```cpp
bool HerculesControl::push_time_sync() {
    uint8_t  flags = sync_flags();   // adjtimex GPS status
    uint64_t ts_ns = host_time_ns(); // CLOCK_TAI or CLOCK_REALTIME

    uint8_t pkt[10];
    pkt[0] = 0x05;
    pkt[1] = flags;
    put_u64_le(pkt + 2, ts_ns);

    return send_raw(pkt, sizeof(pkt));
}
```

The call is fire-and-forget (UDP, no acknowledgement). The Hercules silently
ignores the message if the packet is < 10 bytes.

---

## TimeSyncPusher — exponential backoff

`lib/include/time_sync_pusher.h` / `lib/src/time_sync_pusher.cpp`

Background `std::thread` that calls `push_time_sync()` on a schedule that
doubles after each successful push, following the NTP poll interval strategy:

```
push count:  0    1    2    3   4+
interval:    1s   2s   4s   8s  8s  ← plateau
```

On send failure (link down): resets immediately to 1 s so the Hercules
re-synchronises quickly after recovery.

### Rationale for 8 s plateau (measured, 2026-05-17)

Measured on actual hardware by comparing pcap capture timestamps against
embedded ASAM-CMP timestamps after a single TIME_SYNC push:

| Quantity | Measured value |
|---|---|
| One-way network delay (Hercules → Linux pcap) | **+0.12 ms** |
| RTI crystal drift rate | **90.8 µs/s = 90.8 ppm fast** |
| Drift over 8 s | **0.73 ms** |
| Drift over 64 s (old plateau) | 5.8 ms |

Optimal interval formula: `T = target_accuracy_ms / drift_ppm × 1e6`

| Target | Optimal interval |
|---|---|
| ±0.5 ms | 5.5 s |
| ±1 ms | **11 s → use 8 s (next power of 2)** |
| ±5 ms | 55 s |

The 8 s plateau gives ≤ 0.73 ms inter-sync drift at negligible UDP cost
(10 bytes every 8 seconds). The old 64 s plateau caused 5.8 ms drift —
acceptable but not optimal for signal correlation across buses.

### Usage

```cpp
HerculesControl ctrl;
ctrl.open("169.254.8.3");   // Hercules IP on the capture network

TimeSyncPusher pusher;
pusher.start(ctrl);          // launches background thread

// pusher and ctrl must share lifetime — stop before ctrl goes out of scope
pusher.stop();               // blocks until thread exits
```

Monitor from another thread:
```cpp
std::printf("last sync interval %d ms  ok=%d\n",
            pusher.last_interval_ms(), pusher.last_push_ok());
```

### Integration in app_example

```cpp
// main.cpp — after opening the Hercules control socket:
HerculesControl ctrl;
ctrl.open("169.254.8.3");
ctrl.send_filter(mlbevo_v8_17::kHerculesFilterSlots,
                 mlbevo_v8_17::kHerculesFilterSlotCount);

TimeSyncPusher pusher;
pusher.start(ctrl);     // time sync runs in background for entire capture

// ... receive and decode ASAM-CMP packets ...

pusher.stop();
```

---

## Testing

### Manual one-shot push (no build needed)

```python
import socket, struct, time, sys

HERCULES_IP = sys.argv[1] if len(sys.argv) > 1 else "169.254.8.3"
ts_ns = time.time_ns()
pkt   = struct.pack("<BBQ", 0x05, 0x00, ts_ns)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(pkt, (HERCULES_IP, 5001))
print(f"TIME_SYNC pushed: {ts_ns} ns  ({ts_ns/1e9:.3f} UTC seconds)")
```

### Verify GPS lock status

```sh
# Check if kernel PPS is active
adjtimex 2>/dev/null || timedatectl show | grep -E "NTP|Sync"

# STA_PPSSIGNAL = 0x200 in /proc/net/if_inet6 — or use chronyc:
chronyc sources -v | grep -E "GPS|\*"
```

### Full pipeline test with delta check

After pushing TIME_SYNC, listen for ASAM-CMP FlexRay packets and verify
the embedded timestamp matches the host clock:

```python
import socket, struct, time

HERCULES_IP = "169.254.8.3"

# 1. Push sync
ts_push = time.time_ns()
ctrl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
ctrl.sendto(struct.pack("<BBQ", 0x05, 0x00, ts_push), (HERCULES_IP, 5001))

# 2. Read one FlexRay CMP packet
rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx.bind(("0.0.0.0", 5020))
rx.settimeout(5.0)
data, _ = rx.recvfrom(512)

# CMP DataMessageHeader.timestamp is at bytes 8-15, big-endian
cmp_ts = struct.unpack_from(">Q", data, 8)[0]
ts_now = time.time_ns()

print(f"push ts  : {ts_push} ns")
print(f"CMP ts   : {cmp_ts}  ns")
print(f"host now : {ts_now}  ns")
print(f"delta    : {(cmp_ts - ts_push)/1e6:+.1f} ms")

# Pass if CMP timestamp is within 5 ms of the push and hasn't drifted
assert abs(cmp_ts - ts_now) < 5_000_000, \
    f"timestamp out of sync: delta={abs(cmp_ts-ts_now)/1e6:.1f} ms"
print("PASS")
```

---

## Limitations

| Limitation | Impact |
|---|---|
| No RTT compensation | One-way delay (< 0.1 ms on direct link) is absorbed into the ±0.5 ms RTI quantisation |
| Single push per interval | A lost packet keeps the Hercules at its last anchor; next push corrects it |
| No sync ACK from Hercules | Cannot distinguish "push received" from "push lost"; use `STATUS_REQ` + timestamp comparison for confirmation |
| `absoluteTimeInMilliseconds` is 32-bit on wire | Rolls over at ~49 days; anchor is reset by each push |

# rtdb — Remote Theia Debug Bridge (tdb over gRPC)

`rtdb` is `tdb`'s twin for **remote** operation. Same verbs, same output —
but driven over **gRPC** to `services/com`'s `SupervisorView` instead of local
TIPC, so an operator outside the DMZ can observe + control the system over an
IP connection. `com` is the gRPC↔Theia proxy; `rtdb` is its client.

The command + render layer is **shared** with `tdb` (`tools/tdb/tdb_commands`).
`rtdb ps` and `tdb ps` print byte-for-byte identically — only the transport
client differs (`rtdb_client.SupervisorClient` mirrors `tdb_client`'s method
surface over gRPC).

## Quick start

```bash
# 1. Generate the gRPC stubs (one-time; re-run if a .proto changes).
./tools/rtdb/gen_protos.sh

# 2. Use it (env.sh puts `rtdb` on PATH alongside `tdb`).
rtdb ps                          # live tree (against 127.0.0.1:7700)
rtdb --target 10.0.0.5:7700 ps   # a remote machine's com
rtdb ps --follow                 # poll-stream the tree (Subscribe)
rtdb supervisor                  # host facts
rtdb loglevel sm debug           # set a node's log level live
rtdb trace sm CAST_OUT           # enable a trace kind (control path)
rtdb restart p3                  # child lifecycle
rtdb                             # interactive REPL
```

## Verbs

Identical to `tdb`, **minus `get-snapshot`** (per snapshots are a TIPC/artheia
path with no gRPC surface). `--target host:port` selects the `com` endpoint
(default `127.0.0.1:7700`).

| verb | purpose |
|---|---|
| `ps [--follow [s]]` | live supervisor tree (GetTree poll via Subscribe) |
| `supervisor` / `info` | host facts (GetSystemInfo) |
| `trace [off] <node> [KIND]` | ConfigureTrace — rtdb → com → supervisor → node |
| `trace-config` | stored trace config (GetTraceConfig) |
| `loglevel [<node> [lvl]]` | read / set a node's log level (live) |
| `logcat [--json]` | follow the trace stream (collector TraceStream gRPC) |
| `restart <name>` / `terminate <name>` | child lifecycle |

## How the wire path looks

```
+--------+  gRPC :7700     +---------------+   AF_TIPC      +------------+
|  rtdb  | ──────────────▶ │  services/com │ ─────────────▶ │ supervisor │
| python │                 │  (C++ proxy)  │   localhost    │  (C++)     │
+--------+                 +---------------+                +------------+
```

The live tree is a **poll**, not a push: the supervisor's event firehose is
in-process only (no remote egress), so `com`'s `Subscribe` polls `GetTree` and
streams each `TreeSnapshot` — exactly what `tdb ps --follow` does locally.

`logcat` subscribes to the collector's own `TraceStream` gRPC (default
`:7710`, `services/log` egress-direct); that endpoint moves into a `com`
`TraceForwarder` runnable in a later phase.

## Regenerating stubs

`./tools/rtdb/gen_protos.sh` runs `grpc_tools.protoc` against
`services/com/proto/supervisor_bridge.proto` +
`platform/proto/system/supervisor/supervisor.proto` +
`platform/runtime/proto/platform_runtime/runtime.proto`
(+ `services/log/proto/trace_stream.proto` when present), dropping
`*_pb2.py` / `*_pb2_grpc.py` under `tools/rtdb/_gen/` (gitignored). Re-run
after any `.art` edit that changes a supervisor message type.

## Library usage (drive from tests)

```python
import sys; sys.path.insert(0, "tools/rtdb"); sys.path.insert(0, "tools/tdb")
from rtdb_client import SupervisorClient

c = SupervisorClient("127.0.0.1:7700")
snap = c.get_tree(timeout=3)
assert any(ch.name == "p1" for ch in snap.children)
c.restart_child("p1")
c.stop()
```

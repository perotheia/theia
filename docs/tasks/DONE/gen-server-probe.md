# `artheia.gen_server.probe` — a Python gen_server probe to test FCs in isolation

> **DONE (2026-06-04).** Implemented and in active use: `artheia.gen_server.probe`
> binds a node's TIPC addr from the parsed `.art` and speaks the gen_server wire
> (cast/call) to drive FCs in isolation. It's the transport under tdb
> (`tdb.art` → SupervisorCtl/log) and the observer, exercised live every session
> (ps/info/loglevel/trace/logcat). Connect resilience to stale co-bindings was
> hardened this session (transport.py retry-on-fresh-socket).

> **IMPLEMENTED (2026-05-30..31).** Package: `artheia/artheia/gen_server/probe/`
> (`wire.py` · `codec.py` · `transport.py` · `context.py` · `node.py`). All
> gen_server ops: active `cast`/`call`; passive `on_cast`/`on_call`/`expect_cast`/
> `expect_call`. Verified end-to-end against the real demo:
> `demo/test/probe_loopback.py` (probe↔probe), `demo/test/probe_expect_call.py`
> (passive `expect_call` captures request + replies `value=99`),
> `demo/test/probe_vs_cpp.py` (probe drives the live C++ CounterNode:
> `call(Get)→50`, `cast(Inc{7})→57`). Fully generic — no demo knowledge in the
> package; tests feed it the demo `.art` as spec input only. Native Python
> AF_TIPC tuples (`TIPC_ADDR_NAMESEQ` bind / `TIPC_ADDR_NAME` connect).
> Multi-node FC environments are deferred to rf-theia integration (a later task).


## Goal

Given a node's `.art` spec, stand up a Python process that **mocks a real C++
node**: it binds that node's TIPC address and speaks the Theia gen_server wire
protocol (cast / call / call-reply) byte-for-byte. Point a probe at a
Functional Cluster (FC) under test, or surround one FC with **several probes**
(each impersonating a peer), and you can exercise every message interface the
FC declares — no other C++ nodes, no supervisor, no rig.

A probe is the inverse of an FC node: where the FC *implements* `handle_cast`/
`handle_call`, the probe *drives and asserts on* them — it sends the casts the
FC's receiver ports expect, answers the calls the FC's client ports make, and
captures what the FC sends so a test can assert on it.

## Design decisions (locked)

- **Self-contained, clean slate.** New `artheia/artheia/probe/` package. NO
  dependency on or reuse of the parked C++ `ProbeDaemon`, rf-theia's
  `SmProber`, or `sm_stub`. rf-theia will *consume* `artheia.probe` later (its
  `PortSpec(kind="tipc")` becomes a thin adapter over this); that integration
  is a separate task. No unit tests in this task — the probe IS test
  infrastructure; it gets exercised when rf drives it.
- **Built dynamically from the artheia parser.** A probe is constructed from a
  parsed `.art` model + a node name. Everything wire-relevant (TIPC type/
  instance, port directions, message types, proto type names → service_ids) is
  derived at construction from the parse — no codegen step, no generated files.
- **protobuf 6 only, no gRPC.** Payloads are encoded/decoded with the standard
  `google.protobuf` runtime (v6.x, in the workspace `.venv`). The 24-byte
  `TheiaMsgHeader` is hand-packed with `struct`. **Lazy proto compilation**:
  the `_pb2` module for a message's package is compiled on first use (via
  `grpc_tools.protoc` as the compiler only — emits `_pb2`, never `_pb2_grpc`)
  and cached. No gRPC runtime is imported.
- **Shared `ArtheiaContext`.** An optional object holding one parsed model + the
  compiled-proto cache + the catalog, so several node-probes around one FC
  reuse the same parse and codecs (the "build an env for an FC" case).

## The wire (verified against platform/runtime)

Frame = `TheiaMsgHeader` (24 B, packed, little-endian) followed by
`proto_len` bytes of proto3 payload, over **`AF_TIPC` / `SOCK_SEQPACKET`**,
`TIPC_SERVICE_ADDR` / `TIPC_NODE_SCOPE`.

```
struct '<BBHQHHIH2s'   # exactly 24 bytes
  [0]    bus_type        uint8   = 2  (kBusTypeRpc)
  [1]    msg_type        uint8   = 0x20 cast | 0x21 call | 0x22 call-reply
  [2:4]  proto_len       uint16  payload byte count
  [4:12] timestamp_ns    uint64  (probe may stamp; FC tracer reads it)
  [12:14] service_id     uint16  djb2_low16(proto_type_name)
  [14:16] method_id      uint16  = 0
  [16:20] correlation_id uint32  call: probe-assigned; reply echoes it; cast: 0
  [20:22] seq_num        uint16
  [22:24] reserved       2 bytes
```

`service_id = djb2_low16(name)` where `name` is the nanopb type name — the
flattened proto package + message name, e.g. `system_demo_Inc`:

```python
def djb2_low16(s: str) -> int:        # matches RemoteCodec.hh hash_msg_type_
    h = 5381
    for b in s.encode():
        h = (h * 33 + b) & 0xFFFFFFFF
    return h & 0xFFFF
```

The proto type name is reproduced from the parse exactly as `fc_app.py`'s
`_proto_type_of` does: `_proto_package_name(defining_pkg).replace(".","_") +
"_" + msg.name`. Reuse those helpers directly — do NOT re-derive.

## Direction semantics (who sends what)

For the node the probe impersonates, its ports tell the probe its OWN role; to
drive a *target FC*, the probe plays the COMPLEMENT of the FC's port:

| FC port | FC role | probe (peer) does |
| --- | --- | --- |
| `receiver X requires Iface` | FC receives casts | probe **casts** each `data` msg to the FC |
| `sender X provides Iface` | FC sends casts | probe **binds + receives**, asserts on what arrives |
| `server X provides Iface` | FC answers calls | probe **calls** an operation, awaits the reply |
| `client X requires Iface` | FC makes calls | probe **binds + answers** the call (canned/scripted reply) |

The probe binds the *impersonated peer's* TIPC address; it connects to the
*FC's* address (from the netgraph/`.art` tipc) to cast/call into it; and it
listens on its own bound socket to receive what the FC sends back or initiates.

## Package layout

```
artheia/artheia/probe/
  __init__.py        NodeProbe, ArtheiaContext, public API
  context.py         ArtheiaContext — parse + codec cache shared across probes
  codec.py           proto type name → service_id; lazy .proto → _pb2 compile;
                     encode(msg_name, **fields) / decode(msg_name, bytes)
  wire.py            TheiaMsgHeader pack/unpack (struct '<BBHQHHIH2s'); consts
  transport.py       AF_TIPC SOCK_SEQPACKET: bind/listen/accept (server side),
                     connect (client side), framed send/recv, a select loop
  node.py            NodeProbe — the gen_server engine (below)
```

### `ArtheiaContext`
```python
ctx = ArtheiaContext(art_file, proto_root="platform/proto", catalog=None)
#   .model            parsed once (loader.parse_file)
#   .node(name)       -> NodeView (tipc, ports, per-port msg/proto-type/service_id)
#   .codec            lazy _pb2 cache, keyed by message's defining package
```
Several `NodeProbe`s share one `ctx` → one parse, one proto-compile pass.

### `NodeProbe` — the gen_server engine
```python
probe = ctx.probe("DriverNode")        # impersonate this node, bind its TIPC addr
probe.start()                          # bind SOCK_SEQPACKET, spawn select thread

# --- drive an FC (probe is the active peer) ---
probe.cast("CounterNode", "Inc", n=5)            # frame kMsgGenCast -> FC addr
reply = probe.call("CounterNode", "Get", timeout=2.0)   # kMsgGenCall, await reply
assert reply["value"] == 50

# --- mock a peer the FC talks TO (probe is passive) ---
probe.on_call("Get", lambda req: {"value": 42})  # answer the FC's client calls
probe.on_cast("Inc", handler)                     # react to casts the FC sends

# --- assert on what the FC sent us ---
msg = probe.expect_cast("SpeedSignal", timeout=1.0)   # block until arrives / raise
probe.stop()
```

Internals:
- **start()**: `socket(AF_TIPC, SOCK_SEQPACKET)`, fill `sockaddr_tipc`
  (`TIPC_SERVICE_ADDR`, `TIPC_NODE_SCOPE`, type/instance from the NodeView),
  `bind` + `listen`, run a `select`/epoll loop on a thread.
- **inbound**: read 24-byte header, dispatch by `service_id`. A `kMsgGenCall`
  → look up the registered `on_call` responder, encode its return, frame a
  `kMsgGenCallReply` echoing `correlation_id`, send on the same fd. A
  `kMsgGenCast` → `on_cast` handler + push onto an inbox for `expect_cast`.
- **outbound cast/call**: connect a client `SOCK_SEQPACKET` to the target's
  TIPC addr (cache the fd per target); frame header + payload; for `call`,
  assign a fresh `correlation_id`, register a pending future, and the select
  loop fulfils it on the matching `kMsgGenCallReply`.
- Target addresses resolve from the `ctx` netgraph/`.art` by node name, so
  tests name peers, not hex addresses.

## Multi-probe environment for an FC

```python
ctx = ArtheiaContext("demo/system/demo/component.art")
counter = launch_fc("CounterNode")           # the C++ FC under test (real binary)
driver  = ctx.probe("DriverNode");  driver.start()
ticker  = ctx.probe("TickerNode");  ticker.start()
# exercise:
driver.cast("CounterNode", "Inc", n=5)
assert driver.call("CounterNode", "Get")["value"] == 5
```
Each probe binds a distinct TIPC address (the impersonated node's), so the FC's
`RemoteRef`s connect to the probes exactly as they would to the real peers —
the FC cannot tell the difference (attribution only; no address takeover).

## Open mechanics (resolve during impl)

- **Lazy compile**: `grpc_tools.protoc.main(["", "-I<proto_root>",
  "--python_out=<cache>", "<pkg-subdir>/<leaf>.proto"])`, then import the
  emitted `_pb2`. `package_subdir(pkg)` + `_proto_package_name` give the path
  and the `_pb2` message-class names. Cache per defining-package.
- **`timestamp_ns`**: probe stamps `time.time_ns()` on outbound; harmless to
  the FC, lets the tracer attribute probe traffic.
- **SEQPACKET vs connection reuse**: cache one client fd per target; the FC's
  `TipcMux` accepts and keeps it. Reply fd for an inbound call is the accepted
  socket — reply on the SAME fd.
- **256-byte payload cap**: `TipcClient::send_frame` caps payload at 256 B
  today; the probe should match (or we lift the cap in runtime if a test needs
  bigger). Note it; don't silently exceed.

## Why this fits

- Reuses the artheia parser + `_proto_type_of`/`_proto_package_name` as the
  single source of truth for addresses and service_ids — a probe can never
  drift from what `gen-app` emitted into the FC.
- No new wire, no new codegen: the FC and the probe meet on the exact same
  `TheiaMsgHeader` + proto3 bytes the runtime already defines
  ([[project-theia-wire-header]]).
- Clean slate supersedes the parked C++ `ProbeDaemon` (which needed the now-
  removed `fallthrough`/InfoMsg path); a Python probe needs no in-tree node,
  no fallthrough — it just binds the address and speaks the protocol.
```

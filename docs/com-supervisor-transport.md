# com ↔ supervisor transport

How the GUI / supdbg reach the supervisor, and how the link between
`services/com` and `platform/supervisor` is built on the **standard Theia
transport** (`platform/runtime`: `TipcMux` + `RemoteRef` + `RemoteCodec`,
nanopb `GwMessageHeader` over TIPC) — *not* a hand-rolled wire.

> Status: the control path (request/reply, incl. trace set/get) is on the
> standard transport. The streaming firehose (events/health/snapshot) is a
> separate concern — see §6. A future "envelope-free" com is sketched in §7.

---

## 1. The two protobuf worlds, and why they interoperate

There are two protobuf *runtimes* in play, on purpose:

| | who | encoding | why |
|---|---|---|---|
| **libprotobuf** | GUI, supdbg (gRPC clients), the com gRPC edge | full C++ reflection | rich host tooling; gRPC needs it |
| **nanopb** | the supervisor + every FC on the TIPC wire | fixed structs, no heap | embedded-grade; Hercules/RPi4 |

The load-bearing fact: **proto wire-v3 is identical between them.** A message
encoded by libprotobuf decodes byte-for-byte in nanopb and vice-versa
(confirmed locally, both directions, for a nested `ControlRequest`). So com
does **not** translate field-by-field — it moves the *same payload bytes*
between the gRPC edge and the TIPC wire. libprotobuf lives only at the gRPC
face; the wire is pure nanopb.

---

## 2. What gRPC actually sends on the network

gRPC is **HTTP/2 carrying a length-prefixed protobuf payload**. One unary
call on the wire:

```
HTTP/2 HEADERS frame
   :method = POST
   :path   = /services.com.SupervisorView/ConfigureTrace   ← method = "which op"
   content-type = application/grpc+proto
HTTP/2 DATA frame(s)  — the "Length-Prefixed-Message"
   ┌───────┬────────────────┬───────────────────────────────┐
   │  1 B  │      4 B        │        <len> bytes            │
   │compr  │  msg length     │   PROTO WIRE-V3 PAYLOAD       │  ← the message
   │ flag  │  (big-endian)   │   == SerializeAsString output │
   └───────┴────────────────┴───────────────────────────────┘
HTTP/2 HEADERS frame (trailers)
   grpc-status = 0
```

The **5-byte prefix** (1 compression flag + 4-byte big-endian length) is "the
gRPC framing." Strip it and the body is exactly `SerializeAsString()`'s
output — the same bytes nanopb reads. That is why the two worlds interoperate.

> The gRPC C++ library hands the application the **deframed** payload — either
> as a parsed message (the recoding point: `ParseFromZeroCopyStream` in
> `grpcpp/impl/codegen/proto_utils.h`) or, via the `ByteBuffer` escape hatch,
> as raw bytes. See §7.

---

## 3. The standard Theia transport (the TIPC wire)

Defined by `platform/runtime` + `gateway/libs/libgw`. A frame is a 24-byte
`GwMessageHeader` followed by the nanopb payload:

```
GwMessageHeader (24 B, gw_proto.h)        + nanopb body
  bus_type       = GW_BUS_TYPE_RPC
  msg_type       = GW_MSG_GEN_CALL | _CALL_REPLY | _CAST
  proto_len      = body length
  rpc.service_id = djb2_low16("<nanopb C type name>")   ← "which op/message"
  rpc.method_id  = 0
  rpc.correlation_id                                     ← matches reply↔request
```

- A node *binds* a TIPC name with `TipcMux::bind_node(node, type, instance)`
  and registers handlers: `register_call<Req,Reply>` (clientServer) /
  `register_cast<Msg>` (senderReceiver). Dispatch is by `service_id`.
- A *client* uses `RemoteRef<Node, type, instance>`; the synchronous
  `call(ref, req, act, timeout)` free function sends `GW_MSG_GEN_CALL` and
  blocks for the `GW_MSG_GEN_CALL_REPLY` matched by `correlation_id`. The
  client's own `TipcMux` demuxes the reply (`watch_remote_ref`).

`service_id` here is the same concept as the gRPC `:path`: it says *which
operation*. com's whole job is to translate one routing header (HTTP/2
`:path`) to the other (`GwMessageHeader.service_id`) — the bodies pass
through untouched.

---

## 4. End-to-end: trace set / get (the control path)

```
 GUI / supdbg                    services/com                    platform/supervisor
 (libprotobuf, gRPC)             (gRPC edge + TIPC client)        (gen_server on TipcMux)
 ───────────────────            ─────────────────────────        ────────────────────────
 ConfigureTraceRequest
   .SerializeToString
   gRPC unary  ──HTTP/2──▶  ConfigureTrace(req)  handler
   :path=/…/ConfigureTrace      build ControlRequest{op=9, body}
   [5B][proto body]             cr.SerializeAsString()  (libprotobuf → bytes)
                                       │
                                RemoteRef<SupCtl, 0x80020001, 0>
                                  call(ref, ControlRequest, ⋯, 5s)
                                  GwMessageHeader{GEN_CALL,
                                    service_id=djb2("ControlRequest"),
                                    corr=N}  + the SAME bytes
                                       │  ──TIPC SEQPACKET──▶
                                                                 TipcMux strips header,
                                                                 routes by service_id →
                                                                 register_call<ControlRequest,
                                                                               ControlReply>
                                                                 nanopb pb_decode(bytes)
                                                                   → SupervisorControlNode
                                                                       handle_call → orchestrator
                                                                       (apply_trace_config / …)
                                                                 nanopb pb_encode(ControlReply)
                                  ◀──TIPC GEN_CALL_REPLY{corr=N}──   the reply bytes
                                  call() future wakes (corr match)
                                  ControlReply (nanopb) → fields
   ◀──gRPC reply──  ControlReply (libprotobuf) ParseFromString
 GUI/supdbg reads status / trace_config_list
```

GetTraceConfig is identical, returning the supervisor's stored
`TraceConfigList` inline in `ControlReply.trace_config_list`.

---

## 5. Process shape — supervisor stays an orchestrator, gains a gen_server front-end

The supervisor is **not** rewritten into a single gen_server. Its fork / exec
/ reap orchestration stays on the main `select()` loop. We add a thin
**`SupervisorControlNode`** (a `GenServer`) bound on `TipcMux` at
`0x80020001/0`; its `handle_call<Req,Reply>` thunks into the existing
`Supervisor` orchestrator (start / stop / restart / trace / log). `TipcMux`
runs its own epoll thread; the two share state via the node→`Supervisor`
pointer (mutex where needed). This is the OTP-faithful split: the I/O
front-end is a gen_server, the orchestration is the business logic it calls.

```
              supervisor process
   ┌───────────────────────────────────────────────┐
   │  main thread: select() loop                    │
   │    fork/exec/reap children, restart strategy   │
   │    config_repush_due_, /proc sampler, etc.     │
   │            ▲  (shared state, mutex)             │
   │            │                                    │
   │  TipcMux epoll thread                           │
   │    SupervisorControlNode (GenServer @ 80020001) │
   │      register_call<ControlRequest, ControlReply>│
   └───────────────────────────────────────────────┘
```

The bespoke `TipcPublisher` control path (the `0x0100`/`0x0101` tag frames)
is removed; the firehose tags (§6) stay on `TipcPublisher` for now.

---

## 6. The firehose is a separate concern

`SupervisionEvent` / `HealthBeacon` / `TreeSnapshot` are **broadcast push** to
every subscriber — `TipcMux`'s `register_cast` delivers a `service_id` to
exactly one handler, so it has no multi-subscriber fan-out primitive. The
firehose therefore stays on `TipcPublisher`'s fan-out until a dedicated
migration (smaller nanopb bounds or callback fields for `TreeSnapshot`, which
as a fixed struct exceeds nanopb's 16-bit field descriptors). The control
path swap does not touch it.

---

## 7. Future: envelope-free com as a pure `ByteBuffer` relay

Today com builds a `ControlRequest{op_kind, correlation_id, <body>}` envelope
— and that envelope is the *only* reason com touches protobuf. Because the
gRPC `:path` and the TIPC `service_id` are the same concept, the envelope is
removable:

- each control op becomes its own top-level message keyed by `service_id`
  (`register_call<ConfigureTraceRequest, ControlReply>`, …) — matching the
  `.art` `SupervisorControlIf` operations 1:1; `op_kind` disappears;
- com uses gRPC's **`ByteBuffer`** request/reply (generic service or a
  pass-through `SerializationTraits`): `ByteBuffer::Dump()` → flat bytes →
  relay by `service_id` → reply bytes → `ByteBuffer`. **No `SerializeAsString`,
  no `ParseFromString`, no libprotobuf on the relay path.**

com then becomes a routing shim between two header formats. Caveats:
`correlation_id` moves from the body into the `GwMessageHeader` (check
supdbg/GUI); the firehose (push) is unaffected; the supervisor's nanopb
`pb_decode` becomes the single validator. This is a larger `.art` reshape than
the envelope-kept swap above, so it lands **after** the basic transport swap
is proven by the trace set/get/crash test.

---

## 8. Build

Both ends are Bazel-built via the `rules_foreign_cc` `cmake()` hybrid
(CMakeLists stays the dev source of truth):

- `bazel build //platform/supervisor:supervisor` — the supervisor; consumes
  `//platform/supervisor:supervisor_nanopb` (the nanopb wire binding) +
  `//platform/runtime:runtime` as sandbox deps.
- `bazel build //services/com:services-com` — the bridge; same deps for the
  TIPC client side.

`//platform/supervisor:supervisor_pb_cpp` (libprotobuf) remains for the gRPC /
GUI edge only. nanopb field sizing lives in
`platform/supervisor/system/supervisor.options`.
```
```

### Gate

The `testing/rf_theia/scenarios/services/log/trace_crash_investigation.robot`
scenario (set trace via com → read back via com → crash sm → supervisor
re-applies) is the acceptance gate for the control-path swap.

---

## §6 — Firehose as a topologically-sorted stream (#419)

The monolithic `TreeSnapshot` (one message holding `repeated ChildState`,
×64 nodes each with `threads_detail[16]` + `sockets[16]`) blows past nanopb's
fixed-struct limits (>64 kB), which is why the firehose was the last thing left
on the legacy `TipcPublisher`. We replace it with a **stream of small, fixed
messages over the standard Theia transport** — no message is ever large.

### Wire shape (split: edges vs state)

Two tiny message types, plus a framing op, all cast over the runtime
(GwMessageHeader + nanopb), reusing the same types for full snapshots AND
incremental updates:

- `SnapshotBegin { uint64 generation; uint64 timestamp_ms }` — opens a full walk.
- `NodeEdge { uint32 op; string parent_name; string name; uint32 kind }` —
  one (parent → node) edge. `parent_name == ""` ⇒ root. `op` ∈
  {ADD, REMOVE}. Emitted in **topological order**: a node's edge is sent only
  after its parent's edge already went out (the existing `emit_snapshot` walk
  is already topological — root, then children under known parents).
- `NodeState { string name; sint32 pid; uint32 tid; uint32 state;
  uint32 flags; <resources: cpu_pct, rss_kb, vsz_kb, threads, …> }` — per-node
  mutable state, keyed by `name`. Sent independently of the edge so a
  pid/tid/flag change is one small cast, not a tree rebuild.
- `SnapshotEnd { uint64 generation }` — closes the walk; the receiver swaps the
  rebuilt tree atomically on matching `generation`.

`flags` is a bitmask (`NodeFlag`): `CORE_DUMPED = 1` (last exit dumped core,
set in `reap()` on `WIFSIGNALED` + coredump), `DEGRADED = 2` (supervisor policy,
e.g. restart budget nearing exhaustion), room for more without schema churn.

### Snapshot vs update — same types

- **Full tree**: `SnapshotBegin` → for each node in topo order
  {`NodeEdge(ADD)` + `NodeState`} → `SnapshotEnd`.
- **Incremental** (the common case — a node restarts, gets a new tid; a node
  cores; a node goes degraded): a single `NodeState` cast for the one node. No
  Begin/End, no edges. Tree shape unchanged ⇒ no `NodeEdge`. Node added/removed
  ⇒ a lone `NodeEdge(ADD|REMOVE)` (+ `NodeState` for ADD).

### Transport + threading

The stream casts over the same runtime path as control (the supervisor's
TipcMux), to com's `ComDaemon` `from_sup` receiver, which reassembles and
forwards to the gRPC `Subscribe` stream. This retires `TipcPublisher` and
`services/com/impl/tipc_uplink.cpp` entirely.

The emit + all orchestrator state mutation must be **single-threaded** w.r.t.
the select() loop (the #418 hazard): control CALLs land on the TipcMux epoll
thread, the loop reaps/samples/emits — both touch the tree. Resolve by
serializing mutation onto one owner (a command queue drained by the select
loop, or one supervisor mutex around `do_*`/`apply_*`/`emit_*` + `reap`/`sample`
— never holding it across `fork`).

### Threading: command queue → select loop (chosen)

The select() loop is the SOLE owner of all supervision state (the OTP
"supervisor process"). The control node's `handle_call` runs on the TipcMux
epoll thread and must NOT touch tree state. Instead:

1. `handle_call` builds the nanopb request, pushes `{req, std::promise<reply>}`
   onto a thread-safe command queue, wakes the select loop (self-pipe / eventfd
   added to its `fd_set`), and blocks on the future (bounded timeout).
2. The select loop, each iteration, drains the queue and runs
   `dispatch_control_nanopb` (→ `do_*`/`apply_*`) inline — same thread as
   `reap()`, `sample_procs()`, and the firehose emit. It fulfils each promise.

Result: one writer thread, zero mutexes, no fork-under-lock (`do_start_child`
forks on the loop thread as it always did). The epoll thread only marshals
bytes + waits. The firehose stream is emitted from the loop thread too, so it
always sees a consistent tree.

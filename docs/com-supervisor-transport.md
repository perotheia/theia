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

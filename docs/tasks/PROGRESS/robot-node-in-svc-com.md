Purpose:

1. send signals to component from robot tests
2. Call services from robot test

We read signals in robot from trace massages.
To simulate testing scenarios we need to send signal with fake src field to impersonate node interaction

Tasks:

1. implement signal encoder
   we decoder to json from FFI to libdecoder
   its not enough now.
   instead of extending libdecoder with libencoder - json-\>proto wire
   we proposing to use python protobuf lib to work with signal payload.
   wire format stay the same whenever we use nanopb in cluster or google protobuf on host robot tests.

2. Add robot node to services/com 
- to simulate signals from any component. 
need spetial wiring between com and robot node 
robot node can be disabled in release by injecting conditiona compilation flag from services manifest to bazel to cmake.

---

## Design (#387) — generalize the existing bridge, don't build a new one

Decisions (user):
- **Reuse the com gRPC bridge + its unary call path.** com already
  forwards a ControlRequest envelope to the supervisor over TIPC
  (SupervisorView, request/reply). Piggyback signal-inject as one more
  op on that path — mirror ConfigureLogLevel (#385). No new service,
  no bespoke transport.
- **Generalize the envelope with a destination-node header.** The
  supervisor ControlRequest gets an InjectSignalRequest sub-message
  `{ dst_node, msg_type, payload, src }` + a new op_kind. The supervisor
  routes by op_kind and casts `payload` to `dst_node`'s TIPC name —
  reusing the #386 `send_gw_cast_to_tipc_name` (standard
  GW_MSG_GEN_CAST, no second wire format). `src` is the impersonated
  sender name (rides for trace symmetry / attribution).
- **Drop the FFI; use the standard Python protobuf lib.** rf_theia
  currently decodes trace via ctypes → libtrace_decoder.so. Replace
  encode AND decode with `google.protobuf` over generated `_pb2`
  stubs. Wire format is identical whether nanopb (on-target) or google
  protobuf (host) produced the bytes, so a test builds
  `SmStateMsg(state=RUNNING).SerializeToString()` and the cluster
  decodes it unchanged. The .so stays only as a legacy/fallback.
- **Gate the robot node out of release.** A services-manifest flag
  flows manifest → bazel define / cmake option so the inject op is
  compiled out of a release build (test-only surface).

### Routing fact (decisive) — robot inject == normal port→node routing

The runtime's `register_call` replies on the **socket fd the request
arrived on** (`reply_fd`); the caller's `RemoteRef` correlates the
reply on its OWN persistent client socket by `correlation_id`
(platform/runtime/TipcMux.hh). The receiving component is NOT aware of a
`src` name and does not address the reply by name — it answers on the
connection. So "impersonate a node and get the reply back to the robot"
is the **same mechanism as a normal node→node call**: the robot sender
must be a real connected TIPC client that HOLDS the socket open for the
reply.

Consequence for the two paths:
- **CAST (signal, no reply):** can be one-shot. Either com opens a
  client and sends GW_MSG_GEN_CAST, or it rides the supervisor's
  fire-and-forget `send_gw_cast_to_tipc_name`. No socket to keep.
- **CALL (request→reply):** must NOT go through the supervisor's
  one-shot cast (it closes the socket → reply has nowhere to land).
  **com itself is the robot node**: it opens a persistent TIPC client
  to the TARGET node and does send-GEN_CALL + await-reply-by-corr,
  exactly like `TipcUplink::send_control_request` already does to the
  supervisor. com then returns the reply payload to the gRPC caller.
  This reuses com's existing reader-thread + pending-reply map; no new
  transport.

`src` rides in the envelope for trace attribution / symmetry, but
routing is by the held socket, not by `src` — matching real port→node
routing.

### Flow

```
Robot test (host)
  build payload:  SmStateMsg(state=RUNNING).SerializeToString()   # py proto
  → supdbg/grpc:  com.InjectSignal | com.CallService (dst_node, msg_type, payload, src)
  CAST:  com → (optionally via supervisor) GW_MSG_GEN_CAST → target FC port
  CALL:  com opens TIPC client to target → GW_MSG_GEN_CALL,
         awaits GW_MSG_GEN_CALL_REPLY by corr → returns payload to gRPC caller
  → target FC's TipcMux register_cast/register_call<Msg> → handle_*  # as if from `src`
```

### Status — DONE, verified live against running sm (#387)

All built + tested. A signal driven from the host reached the **real
running sm daemon** and its `handle_call(SmRequest)` ran:
`[sm_daemon] RequestMode(RUNNING) — ...`. The GEN_CALL reply echoed
corr=1 and decoded as SmEmpty. Chain proven end to end: host proto
encode → GW_MSG_GEN_CALL (service_id=djb2("services_services_sm_SmRequest"))
→ sm register_call → handle_call → GEN_CALL_REPLY.

- com proto: `InjectSignal`/`CallService` RPCs + `RobotNode` TIPC client
  (packed GwMessageHeader, GEN_CAST / GEN_CALL + reply-by-corr), gated
  by `THEIA_ROBOT_NODE` (CMake option, ON for dev). com builds.
- gen-app: reporting nodes now `register_call`/`register_cast` their
  declared receiver-port types + emit `DEMO_DECLARE_REMOTE_CODEC` for
  each inbound type. All 5 FCs build + regen byte-stable.
- supdbg: `client.inject_signal()` / `call_service()`; FC `_pb2` stubs
  generated by gen_protos.sh (host encode replaces the FFI).
- rf_theia: `SupervisorClient.inject_signal/call_service` + a
  robot_node selftest (wire contract PASS; live inject+call SKIPs when
  com is down, runs when up).

### Steps (cast + call together)

1. **com bridge proto.** Add two RPCs to supervisor_bridge.proto:
   `InjectSignal(InjectSignalCall) returns Ack` (cast) and
   `CallService(CallServiceCall) returns CallServiceReply` (call).
   Both carry `{ dst_node, msg_type, payload, src }`; the call reply
   carries `{ reply_msg_type, payload }`. **theia (services/com).**
2. **com robot node (the wiring).** com IS the robot node:
   - CAST: open a one-shot TIPC client to dst_node, send
     GW_MSG_GEN_CAST(service_id=djb2(msg_type), payload). (Or reuse the
     supervisor cast for parity — but com-direct keeps it test-local.)
   - CALL: a persistent TIPC client to dst_node, send GW_MSG_GEN_CALL +
     await GW_MSG_GEN_CALL_REPLY by correlation_id — mirror
     `TipcUplink::send_control_request`'s reader-thread + pending map.
     Return the reply payload to the gRPC caller.
   Gated behind the release flag (step 6). **theia (services/com).**
3. **FC receive — THREE dispatch kinds (the "any issues?" answer).**
   An injected message must reach the node via the SAME path a real
   peer uses, which differs by receiver kind:
   - **clientServer operation** (e.g. sm `RequestMode(SmRequest)→SmEmpty`):
     `register_call<Req,Reply>` → `handle_call`. Robot CALL works.
   - **plain senderReceiver `in`** (non-statem FC): `register_cast<Msg>`
     → `handle_cast`. Robot CAST works.
   - **statem event** (e.g. sm `SystemBoot`, `ShutdownRequest`): NOT
     handle_cast — statem ingests via `post_event(server, msg)` (see
     demo/sm_broadcast/test_sm_broadcast.cc). A `register_cast` here
     wouldn't compile (no handle_cast for the event type). So the
     inbound receiver for a statem node must decode then call
     `post_event`, not `handle_cast`.
   gen-app must therefore register the right receiver per declared
   type/port kind: register_call for clientServer ops, register_cast→
   handle_cast for plain senderReceiver, and a register_cast→post_event
   shim for statem events. The runtime already has all three entry
   points; this is template wiring. **artheia templates + 5-FC regen.**
4. **Python encode/decode.** Generate `_pb2` for the FC app protos
   (extend supdbg gen_protos.sh / sibling) and add an rf_theia adapter
   that encodes/decodes signals with google.protobuf — replacing the
   ctypes FFI (libtrace_decoder stays as legacy fallback). Wire format
   identical to nanopb on-target. **theia (tools/supdbg + rf_theia).**
5. **supdbg + Robot surface.** `supdbg signal inject <dst> <msg_type>
   <payload>` + `supdbg signal call <dst> <msg_type> <payload>`
   (+ client methods) and rf_theia keywords (Inject Signal As /
   Call Service As). **theia.**
6. **Release gate.** services-manifest flag → bazel define / cmake
   option compiling the robot-node RPCs out of release. **theia +
   artheia manifest.**
7. **Smoke + build.** bazel build supervisor + com + FC mains;
   fc_regen_stability; a Robot scenario that injects a signal and
   calls a service, asserting the target reacted (via trace) and the
   call returned the expected reply. **theia.**

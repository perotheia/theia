# Theia app trace → Robot Framework — egress-direct, DMZ-governed by com

STATUS: DONE (steps 1-7). Live e2e proven 2/2 — testing/rf_theia/scenarios/
services/log/trace_egress. node → TIPC SOCK_DGRAM → collector in_records →
collector's OWN gRPC TraceStream → rf; control via rf → com.ConfigureTrace
→ supervisor → TraceControlPush. com is a gRPC↔art proxy only (not in the
trace byte path). theia branch trace-egress-collector: 992f80f 87bc039
9d7d9a6 3f2470f 2fb10e7 98ab2df; artheia 27659c9. NOT pushed. Remaining
small fixes (non-blocking): cli.py stale "TraceStream.Configure" docstrings;
system.art stale LogDaemon→TraceCollector forward-decl.

Get a node's trace stream out of the cluster into rf-theia. Builds on the
send-signal probe path (see robot-node-in-svc-com.md) — but the directions
are NOT symmetric, and that asymmetry drives the design.

## The decisive insight (supersedes the old A vs B fork)

The send-signal probe needed com because it is **ingress into the cluster**
— a request crossing the DMZ boundary inward, which must be policed. Traces
are the opposite: **read-only egress of a one-way firehose** (collector →
outside). com proxying that egress buys nothing but an extra TIPC hop and a
second copy of the stream.

ARA's "external ports under com control" means com **governs the DMZ
boundary** — it authorizes, opens, and firewalls external ports. It does
NOT require com to be the byte-pump for every stream. So:

- **services/log[trace] stays a SEPARATE process** (no relocation into com
  — drops the painful collector-into-com migration that the old option A
  proposed).
- **The collector exposes its OWN gRPC hook** with an externally-implied
  protocol, serving the trace stream directly to rf — outside the com proxy.
- **com still controls it in the ARA sense:** com owns the DMZ; it admits +
  firewalls the collector's external port, but is not in the data path.

Split by direction:

```
CONTROL (enable trace — ingress, policed):
  rf → com gRPC ConfigureTrace → supervisor → NodeTraceCtl push → node
       (survives restart: supervisor remembers + re-applies)

EGRESS (bulk read-only stream — direct from producer):
  node → TIPC TraceRecordSubmit → services/log[trace] collector
  rf  → services/log[trace] OWN gRPC hook → TraceRecordStream
        (port admitted + firewalled by com's DMZ control; com NOT in bytes)
```

## Reality check — what is ACTUALLY built (rechecked 2026-05-26)

The earlier design doc over-claimed "#354/#355 built the pipeline; only
wiring + a scenario remain." Rechecking gen-app + the runtime + the
collector against the source:

- **`reporting:true` flag-driven injection (works):** gen-app auto-injects,
  for every reporting node, a config-service receiver — `register_cast<
  LogLevelPush>` + the node's declared inbound types (main.cc.j2:53-92) —
  and the `trace_enable/trace_enabled/trace_clear_all` delegations
  (Daemon.hh.j2:120-130) that flip the process-wide Tracer filter. This is
  the precedent the user means by "use the same flag." (NB: the supervisor
  heartbeat is platform/runtime HeartbeatPublisher.hh, NOT a gen-app
  injection — the template precedent is the config receiver, not heartbeat.)
- **Tracer emits to STDERR ONLY (the real gap):** `Tracer::emit()`
  (Tracer.hh:174) does `fputs(buf, stderr)`. There is NO TIPC submit to the
  collector's `in_records` port. #355 ("FC-side selective trace emit hooks")
  built the per-msg-type FILTER, not the submit TRANSPORT. The .art says so:
  package.art:148-149 — "FC-side wiring is added when the per-FC selective
  emit hook lands." **The egress firehose has no producer feeding it.**
- **com TraceStreamImpl (#354) exists** and bridges the collector's stream
  out over com's gRPC — but per the insight above this proxy is the wrong
  shape for egress; it stays as a fallback, the going-forward egress is the
  collector's own hook.
- **The collector is a gen-app SKELETON:** services/log/{lib,impl,main}
  present, `TraceCollector_handlers.cc` are stubs. It does NOT digest
  netgraph.json and has NO gRPC hook yet.
- **The collector is neither STAGED nor SUPERVISED:** executor.py:74-77
  keeps services/log out of the supervisor tree; demo/stage_local.sh does
  not stage its binary.
- **TraceRecord shape is short:** today `{node_name, msg_type, corr_id,
  ts_ns, payload}` (package.art:93-100). The user wants the binary header to
  carry **src, dst, msg_id, trace_kind** — trace_kind {call/cast, in/out,
  statem} is NEW (the TraceEvent enum Tracer.hh:43-58 has these distinctions
  internally but they never reach the record).

So the live e2e needs real, net-new C++ transport — it is NOT just a Robot
scenario over an existing pipeline.

## Target trace-item framing (user spec)

Each record on the wire = `[trace header][proto]`:

- **src** — source node (component name on egress; address on the bus)
- **dst** — destination node (peer of the traced dispatch)
- **msg_id** — the message/correlation id
- **trace_kind** (NEW) — call | cast, in | out, statem
- **proto** — the original message wire bytes, verbatim

On egress to a gRPC subscriber, the collector converts the **src node
address → component name** using netgraph.json (digested at startup). Local
TIPC subscribers (supdbg) get records **as-is** (addresses, no rename).

When requesting trace from an app, the consumer **specifies the trace_kind**
(so ConfigureTrace gains a kind selector, not just node+msg_type).

## Build steps (ordered; the bulk is C++ transport)

1. **Extend TraceRecord + the Tracer header (artheia .art + runtime).**
   Add `src, dst, msg_id, trace_kind` to `message TraceRecord`
   (services/log/system/package.art) and a `TraceKind` enum {call/cast ×
   in/out, statem}. Map the existing `TraceEvent` (Tracer.hh) onto
   trace_kind at emit time. Regen log proto.
2. **Tracer → TIPC submit (platform/runtime — the missing producer).**
   When `enabled()` and the filter passes, `emit()` ALSO frames
   `[trace header][proto]` and casts it to the collector's `in_records`
   (TIPC 0x80010013) via a per-process submit client — alongside the
   existing stderr line (stderr stays for standalone runs). One submit
   client per process, lazily bound, addressed from a constant (later from
   netgraph). Keep the disabled fast-path a single relaxed atomic load.
3. **`reporting:true` auto-injects the submit wiring (gen-app).** Mirror the
   config-receiver injection: a reporting node gets the trace-submit client
   bound in main.cc.j2 (flag-driven, no hand-authored .art port). Regen the
   5 FCs; assert byte-stable except the new injection.
4. **Collector fanout split (services/log impl + main).**
   - `in_records` handler fans every record to local TIPC `stream_out`
     subscribers AS-IS.
   - Collector digests netgraph.json at startup (node addr ↔ component
     name map).
   - Collector's OWN gRPC hook (new, in services/log/main or a sidecar):
     `Subscribe` streams records with src/dst rewritten addr→component
     name; honors a trace_kind filter from the subscribe request.
5. **ConfigureTrace gains trace_kind (artheia .art + supervisor + com).**
   `TraceConfigRequest` + the supervisor's apply/push carry the kind; the
   node's Tracer filter gates by kind too.
6. **Stage + supervise the collector.** Add services/log[trace] to the
   central supervisor tree (executor.py) and to demo/stage_local.sh so
   install/central/bin/log exists and comes up as a supervised child.
7. **rf-theia client + live e2e scenario.** A scenario lib (mirroring
   sm_gate_lib): stage central + start supervisor + start the collector +
   start com bridge; (a) `Configure Trace node=SmDaemon kind=...` via com →
   supervisor → sm; (b) drive an sm transition; (c) subscribe to the
   COLLECTOR's own gRPC hook and assert an sm record arrives, decoded with
   google-protobuf, with src=sm component name + the expected trace_kind.
   Replaces the fictional keywords in scenarios/services/log/
   trace_collection.robot (that file is a forward-looking spec; its
   `Open Trace Stream` / `Drain Trace Records` keywords were never built).

## e2e (what we prove)

1. rf → com `ConfigureTrace(node=sm, kind=…, on)` → supervisor →
   NodeTraceCtl push to sm. sm's Tracer turns on for that kind.
2. sm dispatch emits → `[header(src=sm,dst,msg_id,kind)][proto]` → TIPC
   TraceRecordSubmit → collector `in_records`.
3. rf → collector's OWN gRPC `Subscribe(kind=…)` → receives sm's records,
   src rewritten to component name, decoded with google-protobuf. Assert the
   expected record (e.g. an sm state_transition) arrives.

## Transport + record decisions (locked 2026-05-26)

- **Trace submit socket = TIPC `SOCK_DGRAM`.** The firehose is
  fire-and-forget, lossy-OK egress; a datagram `sendto` to the collector's
  bound name, dropped silently if it can't go. It must NEVER block or
  back-pressure the traced dispatch thread. (Broader bus convention noted by
  the user: `SOCK_RDM` reliable-datagram for cast, `SOCK_SEQPACKET`
  reliable-ordered for call — trace submit deliberately uses the weaker
  `SOCK_DGRAM` because dropping a trace record is fine, stalling a node is
  not.)
- **Binary record ONLY — drop the stderr `TRC v1 …` line.** Single clean
  path. Consequence: scenarios that grep `TRC v1 …` must be reworked off the
  collector record stream:
    - sm_gate `Sm Reached Running` greps `sm_daemon] → RUNNING` — that's the
      statem's OWN log line, NOT a TRC line → SURVIVES.
    - sm_central `Sm Trace Should Show Recv/Dispatch` greps `TRC v1 …` →
      BREAKS; rework to read off the collector stream (exercises the real
      egress path — more honest).
  `THEIA_TRACE=1` standalone (no collector) still binds the submit client;
  the datagram drops harmlessly if no collector is up.

## Field-number contract (step 1 DONE 2026-05-26)

`TraceRecord` lives in TWO .proto files that MUST keep identical field
numbers — the producer serializes via nanopb (log.proto) and com
deserializes via libprotobuf (supervisor_bridge.proto); `rec.ParseFromString`
in services/com/src/main.cpp:263 reads the same bytes:

  1=node_name(src) 2=dst 3=msg_type 4=corr_id(msg_id) 5=ts_ns 6=kind 7=payload

- services/log/system/package.art → `artheia gen-proto-package` →
  platform/proto/system/services/log/log.proto (committed) → nanopb genrule.
- services/com/proto/supervisor_bridge.proto — does NOT redeclare the
  TraceKind enum (don't duplicate decls across stacks). `kind` is a plain
  `uint32`; proto enums varint-encode identically, so the wire is
  byte-compatible and com casts to/from services_services_log_TraceKind at
  the boundary. Cross-decode verified BOTH directions (com uint32 ↔ log
  enum); dst/kind land in the right slots.
- supdbg/rf stubs: `bash tools/supdbg/gen_protos.sh` regenerates all 51.

STEP 1 status: package.art extended (TraceKind enum + src/dst/msg_id/kind),
both protos synced, supdbg stubs regenerated, roundtrip + cross-decode green.

## Step 2 status (DONE 2026-05-26) — the producer exists

Tracer::emit() (platform/runtime/include/Tracer.hh) now, in place of the
stderr line:
- maps TraceEvent → TraceKind (trace_kind_of): Send→CastOut, Recv→CastIn,
  Dispatch→CallIn, SendReply/CallResult/CallWait/CallResume→CallOut,
  State*→Statem, else Other;
- hand-encodes a proto3 TraceRecord (fields 1=node_name(src) … 7=payload;
  dst left empty — no call site passes a peer yet);
- submits it via `TraceSubmitter` — ONE process-wide TIPC SOCK_DGRAM socket,
  mutex-guarded, MSG_DONTWAIT|MSG_NOSIGNAL sendto to the collector
  (0x80010013), service_id 0xb17a = djb2("services_services_log_TraceRecord")
  so it lands on the collector's in_records register_cast. Lossy: drops on
  no-collector / would-block, never blocks the dispatch thread.

TraceSubmitter has a test seam (`set_test_sink`) — when set, encoded records
go to an in-process sink instead of TIPC, so unit tests assert the real
encode without AF_TIPC. The 3 trace tests in platform/runtime/test/
test_main.cc were reworked off the dropped `TRC v1` stderr onto this sink +
a proto3 decoder (TraceCapture). `bazel test //platform/runtime:tests` green;
all //services/... build.

**Legacy stderr-trace consumers now dead** (replaced by the collector-gRPC
path in step 7; flagged so they're not forgotten):
- testing/rf_theia/adapters/tracer_jsonl.py + runtime/trace_watcher.py —
  tail a file for `TRC v1` lines. No longer produced.
- scenarios/_selftest/sm_central/sm_central_lib.py `Sm Trace Should Show
  Recv/Dispatch` — greps `TRC v1`. Will break; rework off the collector
  stream (was already noted under the locked decisions).
- Several scenarios reference TRC v1 (robot_node/sm_signal_e2e, temporal_
  logic, _phase1_archive) — audit during step 7.

## Step 3 status (DONE 2026-05-26) — reporting gate, NO gen-app change

Turned out to be a runtime-base change, not a template change: the trace
submit already lives in the runtime base (universal), so #401 is about
GATING it, not injecting it. `Tracer` gained `reporting_` (default false) +
`set_reporting()`; `emit()` skips the submit when not reporting (even under
THEIA_TRACE=1). `GenServer<Derived,StateT>`'s constructor calls
`mark_reporting_()` → `tracer_for(kNodeName).set_reporting(Derived::
kReporting)`, SFINAE-detecting kReporting (default false for fixtures /
ad-hoc nodes without it). GenStateM inherits this via its GenServer base —
both node kinds covered by one ctor change. gen-app already emits
kReporting on every daemon, so NO template change and NO FC regen.

Tests: 3 trace tests mark their fixture tracer reporting (post-construction
— the ctor's mark_reporting_() would otherwise clobber it back to the
fixture's false). `bazel test //platform/runtime:tests` 26/26 green; all
//services/... build.

## Step 4 plan (in progress) — collector fanout + own gRPC, com proxy removed

Decisions locked (2026-05-26):
- Two clean paths: CONTROL = rf→com→supervisor→node (configure trace,
  unchanged); EGRESS = node→log→gRPC→rf (collector serves directly).
- gRPC served from the collector's OWN build (going Bazel; CMake only for
  3pp). gRPC is only reachable in Bazel via rules_foreign_cc cmake() today
  (supervisor-gui pattern) OR genrule+protoc/grpc_cpp_plugin + host grpc++
  linkopts — pick the lighter one during build.
- TraceStream service MOVES out of com → services/log (new
  services/log/proto/trace_stream.proto importing supervisor_bridge.proto
  for TraceRecord/TraceConfigRequest — TraceRecord STAYS in
  supervisor_bridge). com drops the TraceStream service block.
- com's TraceStreamImpl + tipc_trace_uplink.{hpp,cpp} REMOVED (com no longer
  proxies traces — egress-direct). Verify com still builds.
- netgraph addr→component-name: the cluster netgraph (`artheia gen-netgraph
  -R platform/system/system.art`) carries per-node {name, tipc:{type,
  instance}} — 18 nodes — exactly the map. Collector digests it at startup
  with nlohmann/json (header-only, no CMake).

### Step 4 progress (proto move DONE; build-infra remaining)

PROTO MOVE done (not yet built — com src still references removed symbols):
- com supervisor_bridge.proto: added `ConfigureTrace(TraceConfigRequest)`
  to SupervisorView (control path stays on com, op_kind=9); REMOVED the
  `service TraceStream` block (Subscribe moved out, Configure folded into
  SupervisorView). TraceRecord / TraceConfigRequest / (TraceSubscribeRequest
  removed — now in log proto) stay.
- NEW services/log/proto/trace_stream.proto: `package services.log`,
  imports supervisor_bridge.proto, defines TraceSubscribeRequest {kind,
  target_node} + `service TraceStream { Subscribe → stream
  services.com.TraceRecord }`.

com cleanup DONE (built green) — "com is a gRPC↔art-protocol proxy only":
- Relocated Configure → SupervisorViewImpl::ConfigureTrace (op_kind=9,
  kOpConfigureTrace); DELETED TraceStreamImpl + its main wiring + the
  #include; DELETED include/com/tipc_trace_uplink.hpp +
  src/tipc_trace_uplink.cpp; dropped from CMakeLists. `cmake --build` green;
  SupervisorView now exposes ConfigureTrace, no TraceStreamStub.
- supdbg: gen_protos.sh rerun (ConfigureTrace in bridge stub, TraceStreamStub
  gone). client.configure_trace → SupervisorView.ConfigureTrace (control
  path). client.subscribe_traces raises NotImplementedError until the
  collector gRPC endpoint lands (egress moved off com). client imports clean.

collector gRPC DONE (built + live e2e green):
- services/log/src/main.cpp (hand-owned): in ONE process — TIPC SOCK_DGRAM
  receiver on in_records (0x80010013) that parses the proto3-wire
  TraceRecord straight into libprotobuf (no nanopb), rewrites src/dst
  addr→component-name via NetgraphMap (nlohmann/json digest of the cluster
  netgraph.json), publishes to TraceHub (in-proc registry, bounded queue,
  best-effort), and serves gRPC TraceStream.Subscribe with kind +
  target_node filters. Listen :7710, --netgraph PATH.
- services/log/CMakeLists.txt: protoc supervisor_bridge (TraceRecord) +
  trace_stream (service, grpc); links host grpc++/protobuf + nlohmann hdr.
- services/log/BUILD.bazel: rules_foreign_cc cmake() wrapper (supervisor-gui
  pattern). `bazel build //services/log:services-log` → green. Direct
  `cmake -S . -B build` → green.
- gen_protos.sh: now compiles trace_stream.proto → trace_stream_pb2{,_grpc}
  for supdbg/rf.
- LIVE E2E proven: proto3 TraceRecord (service_id 0xb17a, GwHeader-prefixed)
  → TIPC SOCK_DGRAM → collector in_records → TraceHub → gRPC Subscribe →
  subscriber got node=sm_daemon msg=SmStateMsg corr=7 kind=5(STATEM).
- 18 addr→name mappings digested from cluster netgraph at startup.

REMAINING:
- step 5 (DECISION: push kind to the node — control path). BLOCKER FOUND
  2026-05-26: the node-side TraceConfig receiver was never wired. The
  supervisor pushes kTagTraceApplyConfig=0x0300 via send_frame_to_tipc_name
  ([u16 tag][payload], NOT the GwMessageHeader path the config_mux reads),
  and NOTHING on the FC side decodes that frame → the daemons'
  trace_enable() methods are dead-ended (#361/#363 left the node half
  incomplete). So "push kind to node" first requires building the node-side
  trace-config decode (a 0x0300 receiver that decodes TraceConfig and calls
  Tracer kind+msg_type enable) — a detour beyond a kind passthrough. Then:
  supervisor .art TraceConfig gains kind → regen; apply/push_trace_config
  carry kind; Tracer gains a kind filter (today msg_type only); 5-FC
  implications. PAUSED for scoping. (Collector-side kind filter already
  works from step 4 as the interim path.)

  RESOLUTION (user): supervisor↔child talk standard .art messages in
  platform/runtime/system/package.art; gen-app auto-wires them for every
  reporting=true node — follow LogLevelPush exactly. This also FIXES the
  #361/#363 node-side gap by routing trace config through the proven
  LogLevelPush mechanism (drop the dead 0x0300 tag frame).

  DONE so far: platform/runtime/system/package.art gains `TraceKind` enum
  (TK_* mirrors log) + `message TraceControlPush { TraceKind kind; bool
  enabled }` — SCALAR-only (no msg_type string: nanopb string=callback,
  same constraint as LogLevelPush; kind is the selector). runtime.proto
  regenerated (nanopb scalar fields confirmed). NB: gen-proto-package
  emitted to proto/platform/runtime/ — had to copy into the build's
  proto/platform_runtime/ path + remove the stray dir (artheia
  package-dir-collapse quirk).

  step-5 worklist progress:
  DONE (runtime half — builds green //platform/runtime:runtime):
  1. Tracer.hh: kind filter — trace_enable_kind(TraceKind,bool) +
     trace_kind_passes() (atomic bitmask, 0=all) + emit() gates on
     trace_kind_of(kind). kind_mask_ member.
  2. GenServer.hh: handle_cast(platform_runtime_TraceControlPush&) base
     overload (next to LogLevelPush) → enable(true)+trace_enable_kind;
     DEMO_DECLARE_REMOTE_CODEC(platform_runtime_TraceControlPush).
  3. gen-app main.cc.j2 + main.statem.cc.j2: reporting config_mux adds
     register_cast<platform_runtime_TraceControlPush>. (NOT yet regen'd to
     the 5 FCs.)
  PROTO plumbing DONE: supervisor .art TraceConfig gains `uint32 kind=4`;
  gen-proto regen'd TraceConfig.proto (+ siblings).
  REMAINING:
  4. supervisor runtime.cpp: apply_trace_config(.., kind) store+push;
     push_trace_config_to_child sends platform_runtime.TraceControlPush via
     send_gw_cast_to_tipc_name (service_id=djb2("platform_runtime_
     TraceControlPush"), encode {kind varint field1, enabled field2} by
     hand like encode_log_level_push) — REPLACING the dead
     kTagTraceApplyConfig=0x0300 send_frame_to_tipc_name path. CMake rebuild.
  5. com src/main.cpp ConfigureTrace: cfg->set_kind(req->kind()). CMake
     rebuild.
  6. regen 5 FCs (gen-app) so each reporting node register_casts
     TraceControlPush; byte-stable except the new line. Build all FCs.
  7. live verify: push TraceControlPush to sm, assert its tracer kind mask
     flips (only that kind reaches the collector gRPC).
- repoint supdbg client.subscribe_traces to a collector-endpoint channel
  (trace_stream_pb2_grpc.TraceStreamStub on :7710) — currently raises
  NotImplementedError. rf-theia keyword surface is step 7.
- small fix: cli.py trace docstrings still say "TraceStream.Configure"
  (stale; harmless) — tidy when touching cli.
- small fix: system.art forward-decl still LogDaemon (real TraceCollector
  0x80010013) — surfaces stale in cluster netgraph.

Sub-steps (incremental, build-check each):
1. Collector impl: in_records handler fans every record to local TIPC
   stream_out subscribers AS-IS + (later) to the gRPC subscriber set. Digest
   cluster netgraph.json at startup → addr→name map. (Pure Bazel + json.)
2. Proto move: new services/log/proto/trace_stream.proto; com proto drops
   TraceStream; regen supdbg/rf stubs.
3. com removal: delete TraceStreamImpl + tipc_trace_uplink; CMake drop;
   com builds.
4. Collector gRPC Subscribe server (the infra-heavy bit): rewrite src/dst
   addr→name, honor trace_kind filter. Bazel-link grpc++.

## Notes / deferred

- com's TraceStreamImpl (#354) stays as a fallback egress path; the
  going-forward egress is the collector's own hook (this doc).
- The libtrace_decoder.so FFI stays legacy; new decode is google-protobuf
  host-side (per the send-signal work).

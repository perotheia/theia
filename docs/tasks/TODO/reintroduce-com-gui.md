# Re-introduce com + supervisor-gui (remote observability over IP)

**Goal (unchanged from the original design):** let an operator observe + control the
Theia system from OUTSIDE the DMZ over an IP connection. com is the gRPC↔Theia
proxy at the DMZ edge; the GUI is "tdb that works remotely" — a wxWidgets
operator console speaking gRPC to com.

```
   operator workstation (admin host, outside DMZ)
        supervisor-gui  ──gRPC/IP──►  com  ──TIPC──►  supervisor (SupervisorCtl)
                                       │   ComGrpcProxy: control + firehose
                                       └───TIPC──►  log[trace] (TraceStreamSub)
                                           TraceForwarder: trace stream
   (central + compute; tdb -i picks the instance)
```

com is the SINGLE gRPC↔Theia edge for everything an external operator needs:
control + firehose (supervisor) AND trace (log[trace]). Every FC stays pure-TIPC
gen-app.

## The core insight

**com is tdb-over-gRPC.** tdb already proves the EXACT supervisor op surface com
forwards — over the probe/TIPC — and it's live every session:

| tdb (probe/TIPC, Python)        | com (gRPC→RemoteRef, C++)         | supervisor op |
| --- | --- | --- |
| get_tree / get_system_info      | Subscribe snapshot / —            | GetTree / GetSystemInfo |
| restart_child / terminate_hold  | RestartChild / TerminateChild     | op 5 / 6 |
| configure_trace / get_trace_config | ConfigureTrace / GetTraceConfig | op 9 / 10 |
| configure_log_level / get_log_level | ConfigureLogLevel               | op 11 |
| on_edge / on_node_state         | Subscribe (firehose reassembly)   | NodeEdge / NodeState |

So this is NOT a rewrite. com's architecture (two codecs: libprotobuf on the
gRPC face, nanopb on the TIPC wire; sup_link control path; sup_firehose
reassembler) is sound. It DRIFTED off the current supervisor wire while parked.
Re-introduction = re-align com to what tdb's probe targets today, wire the one
missing call, fix the proto package, then unhide + rebuild the GUI.

## What drifted (the impedance mismatch — verified)

1. **Control TIPC address is DEAD.** com/sup_link targets the supervisor control
   node at `0x80020003` ("NOT the publisher's 0x80020001"). But the CURRENT
   supervisor unified ctl+publisher: `SupervisorCtl` binds `0x80020001`
   (system.art). There is no `0x80020003`. → com's control path calls a dead
   address. Fix: target `0x80020001` (what tdb's probe resolves).
2. **Firehose never wired.** `register_firehose_casts(com_daemon_cfg)` is MISSING
   from com/main/main.cc → the supervisor's NodeEdge/NodeState topo-pair stream
   lands on ComDaemon and is dropped (GenServer fallthrough=CRITICAL). → Subscribe
   streams hang. Fix: add the call + the include (the code in
   sup_firehose_register.cc already exists).
3. **Proto package mismatch.** supervisor_bridge.proto is `package services.com`
   and imports `ChildState.proto` etc. "from platform/supervisor" — but the
   supervisor proto is now `package system_supervisor`
   (platform/proto/system/supervisor/supervisor.proto). The bridge's -I roots /
   imports point at the old per-message proto layout. Fix: repoint to the
   consolidated system_supervisor proto. (This is also the //platform/supervisor:
   proto_srcs target the supervisor-gui BUILD references and that no longer
   exists.)
4. **Trace gRPC is in the WRONG place — a half-migrated fork.** There are TWO
   log implementations in the tree:
   - `services/log/main/` — the LIVE gen-app FC: LogDaemon + TraceStreamPump +
     TraceCtl, all PURE TIPC. This is what runs + what tdb logcat consumes (via
     TraceStreamSub.Subscribe over TIPC → artheia.observer).
   - `services/log/src/main.cpp` + `CMakeLists.txt` + `proto/trace_stream.proto`
     — an ORPHANED CMake daemon with its OWN gRPC `TraceStream.Subscribe`
     (:7710), wrapped by a foreign_cc `cmake()` BUILD (same stale pattern as the
     old supervisor-gui). The gen-app migration superseded it but left it in the
     tree.
   Putting a gRPC server IN log duplicates com's DMZ-edge role and drags
   grpc++/CMake into an otherwise-clean TIPC FC. CORRECTED DESIGN: trace-gRPC
   moves into com. log[trace] stays pure-TIPC; com gets a NEW `TraceForwarder`
   runnable that subscribes to log[trace]'s TraceStreamSub over TIPC and forwards
   records over its OWN gRPC stream — exactly mirroring ComGrpcProxy's
   supervisor-firehose forwarding. (The GUI's removed-TraceStream compile error
   then points at com's TraceForwarder service, not a log endpoint.)
5. **com is hidden from the build** (commit 90df135: dropped from
   system/services/cluster.art + services/manifest). Fix: re-add once 1–3 build
   + run clean.
6. **Minor:** machines.yaml → machines.json comments; GUI BUILD references the
   missing //platform/supervisor:proto_srcs.

## What's already RIGHT (don't touch)

- com's two-codec isolation (sup_link nanopb / com_bridge_grpc libprotobuf),
  the grpc++ host-genrule wiring, ComGrpcProxy runnable + gRPC server thread.
- sup_firehose's name-keyed NodeEdge/NodeState reassembler (matches the current
  topo-pair firehose tdb consumes via on_edge/on_node_state).
- The GUI's gRPC client (modern, reconnecting), machine discovery (machines.json
  + per-machine manifest dir), the 7 panels (system/apps/processes/trace/etc.).
- The op surface itself — identical to tdb's, which is live-proven.

## Plan (incremental; com first, GUI second)

### Phase A — com control path back online (the dead-address fix)
1. sup_link: target `0x80020001` (SupervisorCtl), not `0x80020003`. Update the
   constant + the comments. (Confirm against system.art / what tdb resolves.)
2. main.cc: add `#include "impl/sup_firehose_register.hpp"` +
   `register_firehose_casts(com_daemon_cfg);` after the ComDaemon bind.
3. Proto: repoint supervisor_bridge.proto's supervisor-message imports to the
   consolidated `system_supervisor` proto (platform/proto/system/supervisor/).
   Fix the genrule -I roots in services/com/BUILD.bazel + the supervisor-gui
   //platform/supervisor:proto_srcs reference (point at the real proto target).
4. Build `//services/com/main:com` standalone (still hidden from the cluster).
   Iterate until it builds clean.

### Phase B — com live test against the running supervisor  ✅ DONE
5. Run com against central's supervisor (instance 0). Verify via a gRPC probe
   (grpcurl or a tiny client): Subscribe yields snapshots (firehose reassembles),
   RestartChild/ConfigureTrace forward to the supervisor and return ControlReply.
   This is the "com == tdb over gRPC" equivalence check: `tdb ps` and a gRPC
   Subscribe must show the SAME tree.

   **RESULT (host install/ tree, supervisor instance 0):**
   - Control path GREEN: `ConfigureLogLevel(sm→debug)` over gRPC →
     `status=0 "log level applied"`. (Phase A control wire re-aligned to the
     current per-op typed SupervisorCtl surface.)
   - Subscribe GREEN via the **GetTree POLL** (see Phase D below) — NOT the
     supervisor event firehose. A gRPC `Subscribe` streamed full `TreeSnapshot`
     observations (36 children, every FC + the supervision tree) on a 1s
     interval, identical to `tdb ps`. The "com == tdb over gRPC" equivalence
     holds: same tree, same source call (GetTree).
6. The instance question: com targets instance 0 today. For compute (instance 1),
   com needs the same THEIA_SUPERVISOR_INSTANCE awareness as run-supervisor.sh
   (or one com per machine, like one supervisor per machine). Decide: one com per
   machine (mirrors the supervisor) — simplest, matches the deploy.

### Phase C — trace-gRPC into com (TraceForwarder), delete the CMake log
7. NEW com runnable `TraceForwarder` (own TIPC addr, own gRPC service — distinct
   from ComGrpcProxy; trace is high-volume best-effort vs sync control, and it
   should be independently restartable). It:
   - subscribes to log[trace]'s `TraceStreamSub.Subscribe` over TIPC (long-
     running, the SAME path tdb logcat / artheia.observer uses),
   - re-streams each `TraceRecord` over a gRPC server-streaming socket to the GUI
     / rf-theia.
   Mirror the ComGrpcProxy pattern (TIPC in → gRPC fan-out, mutex-guarded
   subscriber list, a do_start/do_loop/do_stop runnable owning the gRPC server).
   Move `trace_stream.proto` into com's proto set (it defines the gRPC
   TraceStream the GUI calls).
8. DELETE the orphaned CMake log: `services/log/src/`, `services/log/CMakeLists
   .txt`, `services/log/proto/trace_stream.proto` (moved to com), the foreign_cc
   `services/log/BUILD.bazel` → log becomes a clean gen-app FC (main/ only, like
   the others). Verify log still builds + traces still flow to tdb (unchanged —
   the TIPC hub is untouched).

### Phase D — firehose: the PULL model (the supervisor firehose has no egress)  ✅ DONE
**Finding (do NOT touch the supervisor firehose, per user):** the supervisor's
`broadcast_events_edge` iterates an IN-PROCESS `events_edge_subs_` std::function
list. It has NO remote egress — it never casts NodeEdge/NodeState over TIPC to
anyone. tdb never used it either: `tdb ps` uses the `GetTree` one-shot PULL.

**Resolution — mirror tdb's pull, in tdb first then com:**
- `tdb ps --follow [interval]` (tools/tdb/tdb.py): polls `GetTree` on an interval
  (default 1s), clears + re-renders the tree, Ctrl-C to stop. Proves the model.
- com's gRPC `Subscribe` (ComGrpcProxy_handlers.cc): now POLLS
  `SupLink::get_tree()` (new — raw `TreeSnapshot` bytes, mirrors `get_trace_config`)
  every `THEIA_COM_POLL_MS` (default 1000ms) and emits each snapshot as a
  `SupervisorObservation.snapshot`. The dead `SupFirehose`/`register_firehose_casts`
  path is removed from com (the supervisor never cast to ComDaemon anyway). The
  GUI subscribes once and gets live tree frames; it diffs successive snapshots if
  it wants edge/health deltas. No new supervisor plumbing.

### Phase D2 — re-enable com in the build/deploy
9. Re-add com to system/services/cluster.art + services/manifest (reverse the
   90df135 hiding). com becomes a packaged FC again (its .ipk component).
   (cluster.art already re-adds com; manifest/build wiring still to confirm.)
10. com runs as a supervised child on central (+ compute). Its :7700/:7701 gRPC
    is the DMZ-edge endpoint machines.json already advertises (com_endpoint) —
    now serving BOTH SupervisorView (control+tree-poll) and TraceStream (trace).

### Phase E — supervisor-gui rebuild + re-align
11. Fix the GUI's removed-TraceStream dependency: point grpc_client's TraceStream
    stub at com's NEW TraceForwarder gRPC service (Phase C) — same :7700 endpoint
    as SupervisorView, just a different service. (The old GUI already expected
    both on one com endpoint; this restores that.) Update machines.yaml→.json
    comments.
12. Fix the GUI BUILD (//platform/supervisor:proto_srcs → the real
    system_supervisor proto target); get `bazel build //tools/supervisor-gui`
    green (or keep CMake as the canonical build, like before, and just fix the
    proto path).
13. Live: GUI on the admin host → gRPC to com → renders the supervisor tree
    (SupervisorView) + the trace stream (TraceForwarder), drives restart / trace /
    log-level. The remote-observability goal.

## Task docs — filtered

INFORM the plan:
- extend-supervisor-GUI.md — the 11-phase GUI roadmap (minor naming drift: etcd
  state phases are SUPERSEDED — state is the TIPC firehose, not etcd).
- trace-to-rf-via-com.md (DONE) — the egress-direct trace design. PARTIALLY
  SUPERSEDED: it put the trace gRPC IN log[trace] (the orphaned CMake daemon).
  This plan MOVES that gRPC into com (TraceForwarder); log[trace] stays the
  pure-TIPC hub. The TIPC submit/hub/fan-out half (FC → collector → subscribers)
  is unchanged + correct; only the gRPC-server location moves (log → com).
- per-machine-supervisor-instance.md (DONE) — com must be instance-aware (Phase B6).

DO NOT follow (obsolete):
- etcd-state-backbone.md phases 2–5 — supervisor dropped etcd; no GUI-reads-etcd.
- supervisor-gui-tracestream-repair.md "fix" — assumes old proto + the in-log gRPC.
  The real repoint is com's TraceForwarder service (not log, not the old com
  TraceStream).
- GUI-trace-panel-wireshark-style.md phase-2 data source (etcd Watch) — superseded.

## What "firehose" actually is (investigated Phase A)

The firehose = the supervisor's live tree-state STREAM — the same tree `tdb ps`
renders, pushed continuously as small fixed messages: `SnapshotBegin → {NodeEdge,
NodeState}×N → SnapshotEnd` (the #429 topo-pair stream that replaced the old
64KB monolithic TreeSnapshot). Delivery:

  SupervisorCtl `events` broadcast sender  ──(netgraph, standard transport)──►
    com ComDaemon `from_sup` receiver (TIPC 0x80010008)  ──►  SupFirehose
    (process-global singleton; nanopb→reassemble→libprotobuf TreeSnapshot)  ──►
    ComGrpcProxy Subscribe streams (gRPC fan-out)

The messages are theia-native (nanopb over the standard transport). What's NOT
theia-native is the REGISTRATION: `register_firehose_casts(com_daemon_cfg)`
installs custom cast-sink entries on ComDaemon's binding, and it was CALLED FROM
THE GENERATED main.cc — so the gen-app regen (c31d3a3) wiped it. That hand-call-
in-generated-main is the fragile, non-theia-native part.

### The runnable-owned fix (per user)

Runnable nodes own their init in `do_start` (ComGrpcProxy already does: it
builds the gRPC server + starts SupLink there). The firehose receiver
registration belongs in a runnable's `do_start` too — hand-owned, regen-safe by
construction — NOT in the generated main. Symmetric with the future
TraceForwarder (a runnable that owns its log[trace] subscription in do_start).

Knot to resolve in impl: the firehose currently arrives on COMDAEMON's binding
(gen_server, 0x80010008), but the gRPC server is on COMGRPCPROXY (runnable,
0x80010009). Options:
  (i)  ComGrpcProxy's do_start registers the firehose casts on the process mux
       for ComDaemon's address (the registration is address-keyed, not node-
       keyed — it installs InboundEntry on the NodeBinding). Keep ComDaemon as
       the firehose receiver; just move the REGISTER call out of generated main
       into ComGrpcProxy::do_start (which runs after the binds).
  (ii) Make the firehose arrive on ComGrpcProxy directly (give the runnable its
       own from_sup receiver) so one runnable owns subscribe+reassemble+fanout —
       cleanest, matches TraceForwarder, but moves the .art receiver port.
Lean (i) for Phase A (minimal: relocate the existing call into do_start), revisit
(ii) when adding TraceForwarder so both runnables are symmetric.

## Phase A — concrete work (investigated)

A1. **Control address** DONE — sup_link `0x80020003` → `0x80020001` (the gen-app
    SupervisorCtl; supervisor unified ctl+firehose there; matches tdb's probe).

A2. **Firehose registration → ComGrpcProxy::do_start** (runnable-owned, regen-
    safe). register_firehose_casts(NodeBinding*) installs cast-sinks on
    ComDaemon's binding. do_start needs ComDaemon's binding → add a runtime
    accessor `process_mux()->binding_for(type, instance)` (TipcMux has bindings_
    but no public lookup). Then ComGrpcProxy::do_start:
    `register_firehose_casts(process_mux()->binding_for(ComDaemon::kTipcType,
    inst))`. Remove nothing from generated main (the call was already wiped).

A3. **Proto package re-point** (the build blocker — com won't compile without it):
    - com's BUILD deps point at DEAD targets `//platform/supervisor:supervisor_
      {pb_cpp,nanopb,codecs_hdr}` (the old CMake supervisor's protos, deleted).
    - The supervisor messages now live in `//platform/proto/system/supervisor:
      supervisor_pb` (NANOPB exists), package `system_supervisor` (was
      `services_supervisor`).
    - Work: (a) repoint the nanopb deps; (b) ADD a libprotobuf C++ target for
      system_supervisor (the gRPC edge needs the .pb.cc classes — the old
      `supervisor_pb_cpp` no longer exists; new genrule, like com_bridge_grpc's
      host-protoc pattern); (c) rename 43 `services_supervisor_*` →
      `system_supervisor_*` across 6 com impl files (sup_link, sup_firehose,
      sup_firehose_register, ComGrpcProxy_handlers, sup_firehose_test, +hpp);
      (d) supervisor_bridge.proto: `package services.com` import paths → the
      system_supervisor proto.

A4. Build `//services/com/main:com` clean (still hidden from the cluster).

## A3 deepened — sup_link's CONTROL WIRE is for the old supervisor

The proto repoint surfaced the real mismatch: com's sup_link builds ONE
`ControlRequest{op_kind, ...}` envelope + a switch — that was the OLD supervisor's
wire. The CURRENT supervisor (what tdb calls) has PER-OP typed request messages:
  register_call<StartChildRequest,     ControlReply>   "StartChild"
  register_call<DeleteChildRequest,    ControlReply>   "DeleteChild"
  register_call<ChildSelector,         ControlReply>   "RestartChild"/"TerminateChild"
  register_call<Stop,                  ControlReply>   "Stop"
  register_call<ConfigureTraceRequest, ControlReply>   "ConfigureTrace"
  register_call<GetTraceConfigRequest, TraceConfigList> "GetTraceConfig"
  register_call<ConfigureLogLevelRequest, ControlReply> "ConfigureLogLevel"
  register_call<GetTreeRequest,        TreeSnapshot>   "GetTree"
  register_call<GetSystemInfoRequest,  SystemInfo>     "GetSystemInfo"
`system_supervisor_ControlRequest` doesn't even exist in the new proto. So
sup_link's do_call layer needs a REWRITE: each SupLink op builds the SPECIFIC
nanopb request + RemoteRef::call<ReplyType>() — exactly mirroring tdb_client.py's
per-op `self.probe.call("RestartChild", ...)`. This IS "com is tdb-over-gRPC":
sup_link calls the SAME per-op messages tdb does.

## Phase B — live test result (against central's running supervisor)

com runs on the host (gRPC :7702, targets SupervisorCtl 0x80020001/0):
  ✓ ComDaemon + ComGrpcProxy up; firehose cast-sinks installed on ComDaemon;
    control link up (RemoteRef 0x80020001/0); gRPC SupervisorView listening.
  ✓ CONTROL PATH WORKS end-to-end (the "com == tdb over gRPC" proof):
      gRPC GetTraceConfig → com → supervisor → ControlReply (0 configs).
      gRPC ConfigureLogLevel(sm, info) → status=0 "log level applied".
    A gRPC client drove the live supervisor through com exactly like tdb does
    over TIPC. The sup_link per-op rewrite is verified live.
  ✗ FIREHOSE (Subscribe) does NOT flow yet — deadline, no snapshot. ROOT CAUSE:
    the supervisor BROADCASTS the firehose through SupervisorCtl's `events`
    sender to its .art-declared netgraph PEERS, but com is HIDDEN from the
    cluster → there is no `connect` arrow SupervisorCtl.events → com.from_sup, so
    broadcast_events_* has nowhere to send. The control path uses a direct
    RemoteRef (no netgraph), which is why it works standalone; the firehose is
    push and needs the cluster wiring. → Phase C/D: the firehose comes alive once
    com is re-added to the cluster with the events→from_sup connect.

## Status

Phase A DONE — com builds + runs; control path live-proven. Phase B: control ✓,
firehose pending the cluster re-enable (C/D). Next: Phase C (TraceForwarder +
delete CMake log) then D (re-add com to cluster.art/manifest with the
events→from_sup connect — this also lights up the firehose), then E (GUI).

## Phase D — DEEPER than "re-add com": the firehose has no remote egress

Re-adding com to the cluster (DONE) is necessary but NOT sufficient for the
firehose. The real finding: the supervisor's `events` is an IN-PROCESS subscriber
list. `SupervisorCtl::broadcast_events_edge(m)` iterates `events_edge_subs_`
(std::function callbacks registered by LOCAL code via subscribe_events_edge()).
There is:
  - NO remote-subscribe protocol (a remote peer can't register an in-proc cb),
  - NO `cast(*this, edge, netgraph::com)` — the `events` sender port exists in the
    .art but broadcast_events_* never casts to a netgraph peer.
So the firehose has NEVER egressed over TIPC to com. SupervisorCtl_netgraph.hh
confirms: "no .art-declared outbound peers". The control path works (com dials
the supervisor with a RemoteRef); the firehose is the gap.

To light the firehose to com, ONE of:
  (D-a) connect arrow `supervisor_ctl.events to com_daemon.from_sup` in the
        cluster → gen-app emits netgraph::com on SupervisorCtl; THEN the
        supervisor's emit path (impl/core/runtime.cpp + SupervisorCtl_handlers
        broadcast_events_*) must ALSO `cast(*this, m, netgraph::com)` for each
        firehose message — a supervisor impl change. (Prototype-name collision:
        both Supervisor + ComputeSupervisor name their ctl `supervisor_ctl`;
        the connect must disambiguate or the compositions rename.)
  (D-b) a remote SUBSCRIBE op on SupervisorCtl: com calls Subscribe(my_addr);
        the handler registers a subscriber that casts each event to that addr
        over TIPC. More moving parts but matches "subscriber" semantics + lets
        com (and tdb) come/go without a static connect.

This is net-new firehose-to-remote plumbing, not a config flip. Needs a decision
before implementing. Control + ConfigureTrace/LogLevel already work without it.

(history below)
- A1 DONE (control address 0x80020001).
- A3 proto repoint DONE + BUILDS: supervisor_bridge.proto → one `supervisor.proto`
  import + system_supervisor qualifier; com_bridge_grpc_gen genrule protocs the
  bridge + supervisor + runtime .pb.cc (libprotobuf, no dead supervisor_pb_cpp);
  43 `services_supervisor_`→`system_supervisor_` renames; impl/BUILD deps →
  supervisor_lib (nanopb) + com_bridge_grpc (libprotobuf). `//services/com:
  com_bridge_grpc` builds clean.
- NEXT (A3 cont.): rewrite sup_link's per-op control calls to the current
  supervisor's typed request messages (StartChildRequest/ChildSelector/etc.) —
  the same surface tdb proves. Then A2 (firehose→do_start), A4 build com.

com architecture is sound; this is a re-alignment (dead address →
0x80020001, wire the firehose call, fix the proto package), then unhide + rebuild
the GUI. com-first (A→B→C), GUI-second (D). The op surface is tdb's, live-proven.

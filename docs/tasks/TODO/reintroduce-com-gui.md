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

### Phase B — com live test against the running supervisor
5. Run com against central's supervisor (instance 0). Verify via a gRPC probe
   (grpcurl or a tiny client): Subscribe yields snapshots (firehose reassembles),
   RestartChild/ConfigureTrace forward to the supervisor and return ControlReply.
   This is the "com == tdb over gRPC" equivalence check: `tdb ps` and a gRPC
   Subscribe must show the SAME tree.
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

### Phase D — re-enable com in the build/deploy
9. Re-add com to system/services/cluster.art + services/manifest (reverse the
   90df135 hiding). com becomes a packaged FC again (its .ipk component).
10. com runs as a supervised child on central (+ compute). Its :7700/:7701 gRPC
    is the DMZ-edge endpoint machines.json already advertises (com_endpoint) —
    now serving BOTH SupervisorView (control+firehose) and TraceStream (trace).

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

## Status

Planned. com architecture is sound; this is a re-alignment (dead address →
0x80020001, wire the firehose call, fix the proto package), then unhide + rebuild
the GUI. com-first (A→B→C), GUI-second (D). The op surface is tdb's, live-proven.

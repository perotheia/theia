[tag:done] 

# Supervisor migration to gen-app FC (+ protocol audit)

Migrate the supervisor off its bespoke CMake/hand-rolled build onto the
`gen-app --kind fc` model, like every other FC. Split into a **runnable**
(owns children + reacts to SIGNALs) + an **atomic** (Theia comms). Thread-safe
bridge between them. Clean protocol. Deprecate etcd. Bazel-only (no CMake
foreign_cc).

Original preserved at `platform/supervisor.orig/` (user mines it). New tree:
`platform/supervisor/` (system/ copied from .orig).

## Task 5 — protocol audit: IN SCOPE vs SUSPICIOUS

The current `SupervisorControlIf` has 13 operations. Categorized:

### KEEP — core supervision (children + signals + Theia)

| element | why |
| --- | --- |
| `StartChild` / `DeleteChild` / `RestartChild` / `TerminateChild` | child lifecycle — the supervisor's job |
| `SuspendChild` / `ResumeChild` | the composition-isolation hold (this design) |
| `Stop` | orderly shutdown |
| `GetTree` / `GetChild` | topology query (a client/tdb asks the tree) |
| `NodeReportIf` (HeartbeatReport, SendTimeoutReport) | inbound telemetry → watchdog |
| `ChildSpec`/`ChildSelector`/`ChildState`/`StartChildRequest`/`DeleteChildRequest` | the child model these ops carry |
| `SupervisionEvent` + `NodeEdge`/`NodeState`/`SnapshotBegin`/`SnapshotEnd`/`NodeFlag` | event/topo stream a client consumes |

### RESOLVED disposition (user decisions)

**Trace config OWNERSHIP stays in / moves INTO the supervisor — corrected.**
log[trace] CANNOT own per-node trace config: it doesn't know when a component
restarts after a crash, and trace must survive restart — only the supervisor
knows restarts and can re-push. So:

- **`tdb → supervisor → node`** for trace config; supervisor re-pushes on every
  (re)start. log[trace]'s TraceCtl owns only the trace SUBSCRIPTION (tdb +
  observers subscribe to the firehose — Step 2). Two distinct concerns.
- Trace config endpoint ownership = supervisor. **Merge/simplify** the trace
  config messages into the supervisor (keep ConfigureTrace / GetTraceConfig /
  TraceConfig / TraceConfigList).

| element | disposition |
| --- | --- |
| `ConfigureTrace` / `GetTraceConfig` / `TraceConfig` / `TraceConfigList` | **KEEP** in supervisor — per-node trace config, survives restart. Simplify/merge. Caller is tdb (TIPC), not com. |
| `ConfigureLogLevel` / `LogLevelConfig` | **KEEP** per-child log level, but **NO survives-restart** — just send LogLevelConfig to the child (simple push). |
| `GetSystemInfo` / `SystemInfo` / `SocketInfo` / `ThreadSample` | **KEEP as-is** — the GUI returns eventually. Health indicators stay. |
| `TreeSnapshot` (monolithic 2-frame) | **REMOVE** the monolith — keep the `NodeEdge`/`NodeState` pair stream. |
| `HealthBeacon` | **KEEP** — health indicators. |
| **etcd** (`etcd_publisher.{h,cpp}`, `--etcd-endpoints`, `THEIA_ETCD_ENDPOINTS`) | **DEPRECATE** (task 7) — drop entirely. |
| **com/gRPC bridge framing** | **REMOVE** — all-TIPC; tdb is the client. Supervisor only PROVIDES SupervisorControlIf/EventIf over TIPC. |

## Plan (tasks 3,4,6,7)

3. `gen-app --kind fc` the supervisor from component.art (after the cut),
   `--ns system::supervisor` (matches the `system_` tree convention; resolves
   the package break that triggered this).
4. **Two nodes** in the .art:
   - `SupervisorWorker` (runnable) — owns fork/exec of children, the SIGCHLD
     reap loop, the heartbeat watchdog, SIGTERM/SIGINT handling. do_loop owns
     the wait/select.
   - `SupervisorCtl` (atomic) — gen_server handle_call for SupervisorControlIf
     (Start/Stop/Suspend/Resume/GetTree...) + the SupervisorEventIf sender.
5. Protocol cut per the audit above.
6. **Thread-safe bridge** (a process-global, mutex-guarded SupervisorState:
   the child tree + watchdog table) shared by the two nodes — like log[trace]'s
   TraceHub. Redo the impl interfaces around it.
7. Deprecate etcd; bazel-only (drop CMakeLists + the foreign_cc rule; plain
   cc_binary like the other FCs).

## Progress

DONE:

- Steps 1-2: `platform/supervisor` → `supervisor.orig`; fresh `supervisor/`
  with `system/` copied.
- Step 5 audit + the 3 decisions (above). Protocol cut applied to the .art:
  monolithic TreeSnapshot removed from SupervisorEventIf (NodeEdge/NodeState
  pairs added there); ConfigureTrace KEPT in supervisor (it owns per-node trace
  config, survives restart — log[trace] owns only subscription, see
  feedback-trace-config-ownership). ConfigureLogLevel kept (no survives-restart).
  SystemInfo/SocketInfo/ThreadSample/HealthBeacon kept (GUI returns later).
- Step 3-4: `.art` split into SupervisorCtl (atomic — control/events/reports
  ports) + SupervisorWorker (runnable — OS side). gen-app'd to a clean FC:
  `--ns system::supervisor`, proto package `system_supervisor` (RESOLVES the
  services\_→system\_ package break that triggered this migration). No CMake.

REMAINING:

- Step 6: thread-safe SupervisorState bridge (child tree + watchdog table),
  process-global like log[trace]'s TraceHub. Port the hand-written supervision
  logic from supervisor.orig/src/{runtime,control_node}.cpp into the new
  SupervisorWorker (fork/exec/reap/watchdog/signals do_loop) + SupervisorCtl
  (handle_call dispatch over SupervisorState). ~2400 lines to port.
- Step 7: drop etcd (etcd_publisher.{h,cpp} not carried into the new impl) +
  confirm bazel-only (no CMakeLists). main.cc is generated (bazel cc_binary).
- Then: build, wire into platform/system, retarget the SuspendChild/ResumeChild
  C++ work (already in .orig) onto the new impl, e2e test.

## Strategic decision (user): RETIRE com / supervisor-gui / etcd

Not the first-release target — they "messed up the design for weeks." Stay on
**tdb + Robot Framework + FlexRay**. REMOVE the legacy code, don't re-wire it.
See feedback-retire-com-gui-etcd. This simplifies the migration: dangling
dependents get deleted, not re-provided; the com-bridge protocol framing goes
entirely; trace config is tdb→supervisor→node (no com relay).

### Removal scope (the cleanup)

- DELETE `services/com/` (the gRPC bridge) + `tools/supervisor-gui/` (wxWidgets,
  incl. its etcd panels).
- DELETE the CMake-era roots as each FC is on gen-app: DONE for log
  (services/log/{BUILD.bazel,CMakeLists.txt} removed — builds via
  //services/log/main:log); supervisor.orig has the old CMake (gone with .orig).
- log[trace] TraceCtl simplified: dropped to_supervisor + ctl_com + the
  Configure op (subscription-only now). DONE — builds + observer test green.
- etcd: drop from the new supervisor impl (don't port etcd_publisher.*); etcd
  panels die with the gui.
- supdbg (tools/supdbg) → replaced by tdb (Step 5). Clean its com client.
- Clean refs: demo/manifest/rig.py + service/platform manifests (com FC),
  testing/rf_theia/TheiaTestLibrary.py, platform/proto/BUILD.bazel,
  workspace-root BUILD.bazel.

## DONE this turn

- supervisor → .orig; fresh gen-app FC (SupervisorCtl atomic + SupervisorWorker
  runnable), proto `system_supervisor` (resolves the package break), no CMake.
- protocol cut in .art (monolithic TreeSnapshot out of event iface; trace config
  KEPT/owned by supervisor; ConfigureLogLevel no-restart).
- log[trace] decoupled from supervisor/com; builds green.

## KEY FINDING — .orig is ALREADY two-thread (port is repackaging, not rewrite)

`supervisor.orig/src/runtime.cpp` already has the exact architecture the new
design wants:

- `Supervisor::run()` = the select() loop, SOLE owner of supervision state
  (reap/sample/emit/fork/watchdog) → becomes **SupervisorWorker::do_loop()**.
- `Supervisor::dispatch_control_nanopb` runs on the TipcMux epoll thread and
  bridges to the loop via `post_command()` (mutex + eventfd cmd_queue\_) → the
  control ops → **SupervisorCtl::handle_call(...)** delegating to the shared
  Supervisor.
- So the **thread-safe bridge (task 6) already exists** (cmd_queue\_/eventfd +
  the loop being the single state owner). Task 6 = make the `Supervisor`
  object process-global, shared by the two gen-app nodes.

So the port = REPACKAGE the Supervisor class under the gen-app FC shell:

- carry `runtime.{cpp,h}` + `spec.{cpp,h}` (DROP etcd_publisher.*; drop the
  ControlServer/its own TipcMux — gen-app's SupervisorCtl binds the control
  address + does handle_call instead; reuse dispatch_control_nanopb's body).
- SupervisorWorker::do_loop → construct the shared Supervisor + run() its loop.
- SupervisorCtl::handle_call(op) → post_command into the shared Supervisor.
- main.cc is gen-app generated (no bespoke main.cpp / argv etcd flags).
- the SuspendChild/ResumeChild + held work (already in .orig from Step 4) ports
  straight over.

## A — com unlinked at .art level: DONE

`system/services/com` symlink removed; cluster.art dropped the com import +
`composition Com` stub + cluster member. Services cluster = Log/Per/Sm/Ucm/Shwa.
com dir stays on disk, just not in the model. cluster.art parses clean.

## PORT MAP (decided: rip out TipcPublisher + ControlServer; gen-app ports own it)

The .orig had TWO control paths + a firehose, with an ADDRESS COLLISION
(publisher 0x80020001, ControlServer 0x80020003, new SupervisorCtl wants
0x80020001). Resolution: SupervisorCtl (gen-app, 0x80020001) owns BOTH control
(handle_call on `control`) AND event/firehose emit (`events` sender). Drop
TipcPublisher + ControlServer + etcd entirely.

Split runtime.cpp (carried to impl/core/):

- **ENGINE — port intact (transport-free):** all_workers, start_worker,
  start_subtree, stop_worker, shutdown_subtree, on_child_exit,
  record_and_check_restart, restart_all/rest, reap, do_start/delete/restart/
  terminate/suspend/resume_child, check_heartbeats, sample_procs,
  apply_trace_config, push_trace_config_to_child, apply_log_level,
  push_log_level_to_child, post_command/drain_commands. The suspend/resume +
  `held` work from Step 4 is already in here.
- **TRANSPORT EDGES — rewrite onto gen-app:**
  - `run()` → SupervisorWorker::do_loop() (the signalfd+cmd_eventfd select loop;
    keep, drop the ControlServer start/stop + publisher\_.open/poll).
  - `emit_event` / `emit_health` / `emit_snapshot` / `cast_node_state` /
    `emit_tree_stream` → SupervisorCtl's `events` sender (broadcast_events\_*).
    Currently call publisher\_.publish(tag, bytes) + etcd_publisher\_.* → replace
    with the gen-app sender; drop etcd lines.
  - `on_inbound_frame` (legacy tag dispatch: heartbeat/control/systeminfo) +
    `dispatch_control_nanopb` → SupervisorCtl::handle_call(op) → post_command
    into the engine. Heartbeats come in on SupervisorCtl's `reports` receiver.
  - `push_trace_config_to_child` / `push_log_level_to_child` → the worker casts
    to the child's config port (was publisher\_.reply_to/cast).
  - `send_sm_ready` → worker casts SystemBoot/StartupComplete to sm.
- etcd: strip ctor param + EtcdPublisher member + all etcd_publisher\_.* calls.
  runtime.h ctor already de-etcd'd; runtime.cpp ctor body + members remain.

How the two nodes share the engine: a process-global `Supervisor` instance
(the shared state). SupervisorWorker::do_loop() owns it + runs the loop;
SupervisorCtl::handle_call posts commands into it; SupervisorCtl's `events`
sender is handed to the engine for emit. Bridge = the existing
post_command/cmd_eventfd (already thread-safe).

## DONE (this session, GREEN): protocol + model cleanup

- SuspendChild/ResumeChild REMOVED (not OTP-faithful). Stop-and-hold for test
  mocking = TerminateChild with ChildSelector.no_restart=true; StartChild clears
  the hold. (.art updated: op_kind doc, ControlRequest fields, interface ops.)
- TraceConfigRequest MOVED log[trace] → supervisor: supervisor now defines the
  TraceKind enum + TraceConfig (typed-enum, superior to its old raw-uint32).
  log[trace] dropped TraceKind + TraceConfigRequest + the TraceControl interface
  (TraceCtl is subscription-only). See feedback-trace-config-ownership.
- TraceRecord layout fixed: node_name→src, dropped the TraceKind `kind` field
  (decoupled from trace-config). Kept in .art (it's the wire contract — single
  source for the proto + service_id). observer.py TraceRec + the two trace tests
  updated; log builds, trace_collector_fanout + observer_stream PASS.
- com fully retired from the MODEL: system/services/com symlink removed;
  cluster.art (import/stub/member), services/manifest/{service,executor}.py
  (FC list + supervisor child) dropped com; loader.load_all skips a CLUSTERS
  short with no package.art (so the retired com short no longer errors
  generate-manifest); the 2 tests asserting com updated. 158 artheia tests PASS.
- exec component.art: `extern node atomic Supervisor` → `SupervisorCtl` (the
  node rename) + prototype updated.

## PORT DONE — //platform/supervisor/main:supervisor builds + runs green

The coordinated runtime.{h,cpp} transport rewrite + node wiring + BUILD landed.
`bazel build //platform/supervisor/main:supervisor` is GREEN and the binary runs:
with a manifest it builds the engine, forks children, runs the select() loop,
and shuts down cleanly on SIGTERM (rc=0). Verified end-to-end (no TIPC needed).

What changed this pass:

- **Engine is protobuf-free.** `impl/core/runtime.{h,cpp}` dropped ALL libprotobuf
  `*.pb.h`, the `TipcPublisher`/`EtcdPublisher` members, `ControlServer`,
  `on_inbound_frame`/`dispatch_control_nanopb`. The control surface is now plain
  C++ `ctl_*` primitives (start/delete/restart/terminate/suspend/resume child,
  configure trace/log, get system info / trace config, heartbeat / send-timeout
  ingress). Outbound events/health/topo-pairs leave via an `EmitSink` of
  `std::function`s (plain `EventData`/`HealthData`/`EdgeData`/`NodeStateData`
  structs). `emit_snapshot` collapsed to sample+stream (monolithic TreeSnapshot
  + etcd gone). The trace/log push + sm_ready still cast raw GW_MSG_GEN_CAST to
    child/sm TIPC names (unchanged).
- **Two nodes wired over a process-global bridge** (`impl/core/bridge.{h,cpp}`):
  `SupervisorWorker::do_start` loads the manifest (env `THEIA_SUPERVISOR_MANIFEST`
  / `THEIA_ROOT_DIR`), constructs the engine, installs the EmitSink → bridge
  EmitForwarder, publishes it; `do_loop()` == `Supervisor::run()`; `do_stop()`
  == `request_shutdown()`. `SupervisorCtl::handle_call`/`handle_cast` read nanopb
  fields, `post_command` into the engine, block on a `std::promise` for the reply.
  `SupervisorCtl::init` installs the EmitForwarder so engine events fan out via
  this node's `events` broadcast senders.
- **etcd dropped** (never carried). **No CMake** — `impl/BUILD.bazel` hand-owns a
  `supervisor_engine` cc_library (core/ + nlohmann_json) + `supervisor_impl`; the
  generated `main.cc`/lib are untouched.
- **proto wiring**: added `platform/proto/system/supervisor/{BUILD.bazel, supervisor.options}` (nanopb genrule + field-size pins so inbound strings are
  READABLE — see feedback-nanopb-options) + registered in `platform_protos`.
- **C++ namespace = `ara::exec`** (AUTOSAR Execution Management), NOT
  `system::supervisor` — the literal `system` namespace collides with
  `int system(const char*)` from \<cstdlib\>. Proto package stays
  `system_supervisor`. See project-supervisor-namespace-ara-exec.

## REMAINING (next focused efforts)

- **Generator gaps found** (hand-patched for now, fix upstream): (1) the proto
  generator omitted `message Stop {}` for the no-arg `operation Stop()` — added by
  hand to supervisor.proto; (2) gen-app `--ns` must be `ara::exec`, not
  `system::supervisor`; (3) the proto dropped `ChildSelector.no_restart` (only
  `name` survived) — re-added by hand, it's the discriminator between RestartChild
  and TerminateChild (both take ChildSelector → one handle_call overload, branches
  on no_restart: true=terminate+hold, false=restart).
- **Dead monolithic envelope removed** (the "separate messages" cleanup): the old
  `ControlRequest` (op_kind + oneof) + `ControlReply.correlation_id` are gone from
  package.art + supervisor.proto (per-op CALLs correlate at the transport layer,
  not via an envelope id); the unused carried `impl/core/control_node.h` deleted;
  stale op_kind/ControlRequest/phase-3 comments fixed.
- Retarget the SuspendChild/ResumeChild test-mock work: the engine keeps
  `do_suspend_child`/`do_resume_child` + the `held` flag, but the OTP-faithful
  protocol cut REMOVED Suspend/Resume from the wire (stop-and-hold = Terminate
  with no_restart). Confirm tdb drives hold via TerminateChild; the `ctl_suspend/ resume` wrappers are currently unreachable from the wire (kept for a probe).
- Wire the manifest path properly into the rig (today: env var + cwd default).
- GetTree/GetChild handle_call return empty envelopes (the monolithic read died
  with com); a client reads the live tree off the NodeEdge/NodeState firehose.
  Wire tdb to that + an e2e test (build platform/system green first).
- 
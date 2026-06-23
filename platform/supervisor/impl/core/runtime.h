// The Supervisor ENGINE. fork/exec, signal handling, restart logic.
//
// Transport-FREE: this class owns the supervision state (the child tree,
// the watchdog table, the restart strategy) and the select()-loop that
// drives it. It no longer binds any TIPC socket itself. The gen-app FC
// shell wraps it:
//   - SupervisorWorker (runnable) constructs the process-global Supervisor
//     and runs do_loop() == run() (the select loop, sole state owner).
//   - SupervisorCtl (atomic gen_server) receives the control surface over
//     the STANDARD Theia transport (nanopb on TipcMux) and hands each op to
//     this engine as a typed ExecCommand via enqueue()/call() (the actor
//     queue); the loop thread dispatches it.
// Outbound events/health/topo-pairs leave via the EmitSink callbacks below,
// which the FC shell wires to SupervisorCtl's `events` broadcast senders.
// Per-node trace/log config + the SM startup handshake still cast raw
// GW_MSG_GEN_CAST frames straight to the target's TIPC name (see runtime.cpp).

#pragma once

#include "registry.h"
#include "spec.h"

#include "Logger.hh"   // theia::runtime::Logger (the engine's injected sink)

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <utility>
#include <vector>

namespace supervisor {

// ---- Plain (protobuf-free) value types crossing the engine boundary ------
//
// The engine speaks plain C++ structs; the gen-app SupervisorCtl node
// translates these to/from the nanopb system_supervisor_* wire types.

// One supervision event (child started/exited/restart/escalation/watchdog/
// send-timeout). kind ordinals match the old SupervisionEvent.kind table.
struct EventData {
    uint32_t    kind{0};
    uint64_t    timestamp_ms{0};
    std::string child_name;
    std::string supervisor_name;
    int32_t     pid{-1};
    int32_t     exit_code{0};
    std::string strategy;
    std::string tombstone_path;
    std::string detail;
};

struct HealthData {
    uint64_t timestamp_ms{0};
    uint64_t uptime_ms{0};
    uint64_t generation{0};
    uint32_t total_workers{0};
    uint32_t active_workers{0};
    uint64_t total_restarts{0};
    uint64_t total_tombstones{0};
};

// Edge in the topo-pair firehose: op (0=ADD), parent, name, kind (0=worker,
// 1=supervisor).
struct EdgeData {
    uint32_t    op{0};
    std::string parent_name;
    std::string name;
    uint32_t    kind{0};
};

// Flat per-node state in the topo-pair firehose.
struct NodeStateData {
    std::string name;
    int32_t  pid{-1};
    uint32_t tid{0};
    uint32_t state{0};
    uint32_t flags{0};
    uint32_t restart_count{0};
    int32_t  last_exit_code{0};
    uint64_t uptime_ms{0};
    uint32_t cpu_pct{0};
    uint64_t rss_kb{0};
    uint64_t vsz_kb{0};
    uint32_t threads{0};
    uint64_t shared_kb{0};
    uint64_t data_kb{0};
};

// Host facts for GetSystemInfo (replaces the libprotobuf SystemInfo).
// Supervisor IDENTITY only. The host hardware stats (cpu_count/total_ram/
// uptime/disk) moved to services/shwa, the host system-monitor.
struct SystemInfoData {
    std::string hostname;
    std::string kernel;
    std::string os_pretty_name;
    std::string theia_git_sha;
    std::string build_timestamp;
    uint64_t    start_timestamp_ms{0};
};

// One read-back trace-config row for GetTraceConfig.
struct TraceConfigRow {
    std::string target_node;
    uint32_t    kind{0};   // TraceKind ordinal; enabled is implicit (present).
                           // One row per (node, enabled-kind) — a node tracing
                           // several kinds yields several rows.
};

// One read-back log-level row for GetLogLevelConfig (tdb loglevel). Effective
// level = override if set, else boot; is_override flags that.
struct LogLevelRow {
    std::string target_node;
    uint32_t    level{0};        // effective LogLevelValue ordinal (0..4)
    bool        is_override{false};
    uint32_t    boot_level{0};   // manifest THEIA_LOG_LEVEL ordinal
};

// One supervised worker's EXACT log sink for GetLoggerPolicy — the log[]
// hose's "where do I tail this node?". Derived from the worker's THEIA_LOGGER
// spawn env (the value build_supervisor_tree authored, e.g. "file:/var/log/
// theia/sm.log" or "syslog"). The supervisor parses it ONCE here so the hose
// gets the exact path/tag and never guesses.
//   node — the supervised worker name (= the child name).
//   sink — the scheme: "file" | "syslog" | "stdio" | "null".
//   path — for file: the exact log file. Empty otherwise.
//   tag  — for syslog: the ident the node logs under (= the node name today).
struct LoggerEntryRow {
    std::string node;
    std::string sink;
    std::string path;
    std::string tag;
};

// One thread's snapshot in a TreeRow (mirrors the ThreadSample wire message),
// so GetTree carries the same per-thread breakdown the firehose does.
struct TreeThreadRow {
    uint32_t  tid{0};
    std::string comm;
    uint32_t  cpu_pct{0};
    uint32_t  sched_policy{0};
    uint32_t  sched_priority{0};
    int32_t   nice{0};
    uint64_t  cpu_affinity_mask{0};
    uint32_t  last_cpu{0};
};

// One TIPC socket owned by a worker pid — projected into ChildState.sockets.
// Sampled per tick: the TIPC sock_diag dump (inode → queue depths + addrs)
// joined against the pid's /proc/<pid>/fd socket inodes. `rx_queue`/`tx_queue`
// are the kernel's current receive/transmit backlog in bytes (per-socket, not
// cumulative) — a non-zero rx_queue means a node is falling behind on intake.
struct TreeSocketRow {
    uint64_t  inode{0};
    uint32_t  state{0};        // TIPC sk state (kernel enum)
    uint32_t  rx_queue{0};     // bytes queued for receive
    uint32_t  tx_queue{0};     // bytes queued for transmit
    std::string local;         // "type:lower" or ref repr
    std::string remote;        // peer, "" if none
};

// One row of the supervisor tree for GetTree (a flat, parent-keyed list — the
// caller reassembles the hierarchy by name, same shape the firehose streams).
// kind: 0=worker, 1=supervisor. state: 0=stopped, 2=running, 3=terminating.
struct TreeRow {
    std::string name;
    std::string parent_name;
    uint32_t    kind{0};
    int32_t     pid{-1};
    uint32_t    state{0};
    uint32_t    restart_count{0};
    int32_t     last_exit_code{0};
    uint32_t    flags{0};
    std::string strategy;       // supervisors only
    std::string start_cmd;      // workers only
    // Resource metrics (workers only; 0 for supervisors/nodes). Sampled from
    // /proc by the engine each tick (sample_[pid]) — the SAME data the
    // NodeState firehose carries, so GetTree (tdb ps / GUI poll) and the
    // firehose agree. uptime_ms = now - last_start.
    uint64_t    uptime_ms{0};
    uint32_t    cpu_pct{0};      // hundredths of a percent
    uint64_t    rss_kb{0};
    uint64_t    vsz_kb{0};
    uint64_t    shared_kb{0};
    uint64_t    data_kb{0};
    uint32_t    threads{0};
    uint32_t    tipc_rx{0};   // summed TIPC receive-queue bytes (this pid)
    uint32_t    tipc_tx{0};   // summed TIPC transmit-queue bytes
    std::vector<TreeThreadRow> threads_detail;
    std::vector<TreeSocketRow> sockets;   // full per-socket detail (off the wire)
};

// ---- The executor command (actor ingress) --------------------------------
//
// Peer threads (SupervisorCtl on the TipcMux thread) NEVER touch engine state
// directly. They build a typed ExecCommand and hand it to Supervisor::enqueue()
// (fire-and-forget cast) or Supervisor::call() (blocks for a reply). The loop
// thread — the SOLE owner of the tree, fork/exec/waitpid, SIGCHLD, and the
// config tables — drains the queue and dispatches each command via a single
// switch. No state lock: there is one writer.
//
// Flat fields (not a union): only the ones an op needs are populated. Trivially
// movable; the queue owns it by value.
struct ExecReply {
    uint32_t                    status{0};   // ControlReply.status ordinal
    bool                        ok{false};   // configure_* resolve result
    std::vector<TreeRow>        tree;        // GetTree
    std::vector<TraceConfigRow> trace_cfg;   // GetTraceConfig
    std::vector<LogLevelRow>    log_cfg;     // GetLogLevelConfig
    std::vector<LoggerEntryRow> logger_cfg;  // GetLoggerPolicy (per-node sinks)
    std::string                 logger_machine_sink;  // GetLoggerPolicy (the
                                             // un-expanded machine THEIA_LOGGER_POLICY)
    SystemInfoData              sysinfo;     // GetSystemInfo
    // GetTombstone — the crashed child's tombstone text (capped) + metadata.
    bool                        tomb_found{false};
    std::string                 tomb_path;       // on-host path of the file
    std::string                 tomb_content;    // file bytes (already capped)
    uint64_t                    tomb_total{0};   // full file size
    bool                        tomb_truncated{false};
    HealthData                  health;          // GetHealth
};

struct ExecCommand {
    enum class Op {
        StartChild, DeleteChild, RestartChild, SuspendChild, ResumeChild,
        TerminateChild, OnHeartbeat, OnSendTimeout, ConfigureTrace,
        ConfigureLogLevel, GetTree, GetSystemInfo, GetTraceConfig,
        GetLogLevelConfig, GetLoggerPolicy, GetTombstone, GetHealth, Shutdown,
        // process groups (pg): a node joins/leaves a group (= a wire message
        // type) to receive its broadcasts; a broadcaster watches a group to be
        // pushed its membership. Reaped by the watchdog on heartbeat-miss/SIGCHLD.
        PgJoin, PgLeave, PgWatch,
    };
    Op op;

    // ---- typed args (per-op subset) ----
    std::string              name;          // child / target / heartbeat node
    std::string              parent;        // StartChild parent supervisor
    std::vector<std::string> start_cmd;     // StartChild argv
    std::vector<std::string> modules;       // StartChild modules
    int                      restart{0};    // StartChild
    int                      shutdown{0};   // StartChild
    int                      type{0};       // StartChild
    bool                     no_restart{false};  // SuspendChild vs RestartChild caller intent

    pid_t                    pid{-1};       // OnHeartbeat
    uint64_t                 seq{0};        // OnHeartbeat

    std::string              callee;        // OnSendTimeout
    std::string              iface;         // OnSendTimeout
    std::string              method;        // OnSendTimeout
    uint32_t                 budget_ms{0};  // OnSendTimeout
    uint32_t                 observed_ms{0};// OnSendTimeout

    bool                     enabled{false};// ConfigureTrace
    uint32_t                 kind{0};       // ConfigureTrace
    uint32_t                 level{0};      // ConfigureLogLevel

    // pg (PgJoin / PgLeave / PgWatch). group_id = the wire message-type service_id.
    // tipc_type/tipc_instance = the joining member's RECEIVE address (PgJoin only).
    uint32_t                 group_id{0};
    uint32_t                 tipc_type{0};
    uint32_t                 tipc_instance{0};

    // reply channel — set ONLY by call(); null for enqueue() casts.
    std::promise<ExecReply>* reply{nullptr};
};

// The outbound emit surface. The FC shell installs these (each forwards to
// SupervisorCtl's matching `events` broadcast sender); the engine calls them
// from the loop thread. Any unset callback is a quiet no-op (best-effort).
struct EmitSink {
    std::function<void(const EventData&)>     on_event;
    std::function<void(const HealthData&)>    on_health;
    std::function<void()>                     on_snapshot_begin_unused; // reserved
    std::function<void(uint64_t /*generation*/, uint64_t /*ts_ms*/)> on_snapshot_begin;
    std::function<void(const EdgeData&)>      on_edge;
    std::function<void(const NodeStateData&)> on_node_state;
    std::function<void(uint64_t /*generation*/)> on_snapshot_end;
    // Ask CONTROL to push a child's trace/log config — BY NAME, with the typed
    // values. The engine owns the config state but does NOT resolve addresses,
    // encode protobuf, or touch a socket. SupervisorCtl (the runtime-backed
    // node) RESOLVES the child + builds + casts platform_runtime_
    // TraceControlPush / LogLevelPush. kind = TraceKind ordinal; level =
    // LogLevelValue ordinal. (Used by the restart re-push; the live path is the
    // handler calling the same SupervisorCtl::set_* directly.)
    std::function<void(const std::string& /*child*/, uint32_t /*kind*/,
                       bool /*enabled*/)>            set_trace;
    std::function<void(const std::string& /*child*/, uint32_t /*level*/)>
                                                     set_log_level;
    // Ask CONTROL to push a group's membership to ONE watcher (broadcaster). The
    // engine owns the pg registry but does NOT cast: SupervisorCtl builds the
    // PgMembership proto + casts it to the watcher's SupervisorEventIf receiver
    // at (watcher_type, watcher_instance). members = each member's RECEIVE addr.
    struct PgMemberAddr { uint32_t tipc_type; uint32_t tipc_instance; };
    std::function<void(uint32_t /*watcher_type*/, uint32_t /*watcher_instance*/,
                       uint32_t /*group_id*/,
                       const std::vector<PgMemberAddr>& /*members*/)> push_pg;
};

class Supervisor {
public:
    // machine_name: identifies this supervisor (retained for logs/events).
    // Defaults to gethostname() if empty. (etcd dropped — all-internal-TIPC;
    // tdb is the client.)
    Supervisor(std::unique_ptr<Node> root,
                std::string root_dir,
                std::string machine_name   = "");
    ~Supervisor();

    Supervisor(const Supervisor&)            = delete;
    Supervisor& operator=(const Supervisor&) = delete;

    // Install the outbound emit surface (the FC shell wires these to
    // SupervisorCtl's `events` broadcast senders). Call BEFORE run().
    void set_emit_sink(EmitSink sink) { emit_ = std::move(sink); }

    // Install the logger the engine routes ALL its lines through — the owning
    // SupervisorWorker passes &node.log() so engine output wears the
    // [#supervisor_worker] tag. Until set, falls back to process_logger().
    // Call BEFORE run(). The Logger outlives the engine (owned by the node).
    void set_logger(::theia::runtime::Logger* lg) noexcept;

    // Drive the tree. Returns the process exit code. This IS
    // SupervisorWorker::do_loop(): the signalfd + command-queue select loop,
    // sole owner of all supervision state.
    int run();

    // Request graceful shutdown — safe to call from a signal handler or the
    // FC shell's do_stop().
    void request_shutdown();

    // ---- Executor command surface (the ONLY thing peer threads touch) ------
    //
    // SupervisorCtl (TipcMux thread) builds a typed ExecCommand and hands it
    // here. The select() loop is the SOLE owner of all supervision state
    // (tree / fork / waitpid / SIGCHLD / config tables); it drains the queue
    // and dispatches each command via one switch on the loop thread. cmd_mutex_
    // guards ONLY the queue (ingress) — never exposed; there is no state lock.
    //
    //   enqueue(cmd)  — fire-and-forget cast. Returns immediately.
    //   call(cmd)     — blocks on a single per-call std::promise for the reply.
    //
    // Both wake the loop via cmd_eventfd_. Safe from any thread; a no-op if the
    // loop has already shut down (call() then returns a default ExecReply).
    void      enqueue(ExecCommand cmd);
    ExecReply call(ExecCommand cmd);

    // ---- Control-surface primitives — LOOP-THREAD ONLY. Invoked exclusively
    //      by drain_commands()'s dispatch switch (never from a peer thread), so
    //      they need no lock: the loop is the single writer. All take/return
    //      plain C++ values — the nanopb<->engine translation lives in
    //      SupervisorCtl_handlers.cc. Each returns a status ordinal matching
    //      the old ControlReply.status convention.

    // StartChild: hot-add + spawn. status out; returns nothing else needed.
    uint32_t ctl_start_child(const std::string& parent_sup,
                             const std::string& name,
                             const std::vector<std::string>& start_cmd,
                             int restart, int shutdown, int type,
                             const std::vector<std::string>& modules);
    uint32_t ctl_delete_child(const std::string& name);
    uint32_t ctl_restart_child(const std::string& name);
    uint32_t ctl_terminate_child(const std::string& name);
    uint32_t ctl_suspend_child(const std::string& name);
    uint32_t ctl_resume_child(const std::string& name);

    // The immutable name → TIPC address index, built once from the manifest.
    // Read-only and lock-free: SupervisorCtl resolves a trace/log target off it
    // DIRECTLY from its own thread (no command-queue hop) — the data it reads
    // (per-node TIPC addresses) is fixed at load and never mutated by fork/reap
    // or hot-add/delete. See registry.h.
    const Registry& registry() const { return registry_; }

    // ConfigureTrace / ConfigureLogLevel — STORE the config only (survives
    // restart via the spawn env + the heartbeat-after-gap re-emit). They do NOT
    // send: SupervisorCtl resolves + casts. Return false when the target
    // doesn't resolve. level is the platform.runtime LogLevelValue ORDINAL,
    // carried verbatim from the wire; mapped to a name ONCE for the spawn env.
    bool ctl_configure_trace(const std::string& target_node,
                             bool enabled, uint32_t kind);
    bool ctl_configure_log_level(const std::string& target_node,
                                 uint32_t level);

    // GetTraceConfig read-back: flatten the per-child trace_configs_ table.
    std::vector<TraceConfigRow> ctl_get_trace_config();

    // GetLogLevelConfig read-back: EVERY reporting node with its effective log
    // level — boot (the worker's THEIA_LOG_LEVEL spawn env) ⊕ override (the
    // log_levels_ map). For tdb loglevel (no args).
    std::vector<LogLevelRow> ctl_get_log_level();

    // GetLoggerPolicy read-back: EVERY supervised worker with its EXACT log
    // sink, parsed from the worker's THEIA_LOGGER spawn env. The log[logging]
    // hose calls this at init to learn what to tail (the precise <dir>/<node>
    // .log files) or which journald tags to follow — no path guessing. `sink`
    // out-param receives the un-expanded machine THEIA_LOGGER_POLICY.
    std::vector<LoggerEntryRow> ctl_get_logger_policy(std::string& machine_sink);

    // GetTombstone: read a crashed child's tombstone text (capped) into rep.
    // Matches by NAME (newest tombstone, any pid — the child may have restarted
    // since it cored). rep.tomb_found is false when there's no tombstone.
    void ctl_get_tombstone(const std::string& child_name, ExecReply& rep);

    // GetTree: a flat, parent-keyed snapshot of the whole tree (root +
    // supervisors + workers, synthetic <worker>_sup rows for reporting
    // workers). The caller (tdb ps / supervisor) rebuilds the hierarchy.
    std::vector<TreeRow> ctl_get_tree();

    // GetSystemInfo: host facts on demand.
    SystemInfoData ctl_get_system_info();

    // HeartbeatReport ingress (SupervisorCtl::handle_cast posts this).
    void ctl_on_heartbeat(const std::string& node_name, pid_t pid,
                          uint64_t seq);
    // SendTimeoutReport ingress → a kind=7 supervision event.
    void ctl_on_send_timeout(const std::string& caller, const std::string& callee,
                             const std::string& iface, const std::string& method,
                             uint32_t budget_ms, uint32_t observed_ms);

private:
    // Drain + dispatch all queued ExecCommands on the loop thread (one switch).
    // Called once per select() iteration BEFORE reap/sample/emit.
    void drain_commands();

    // The single dispatch point: run one command on the loop thread and, for
    // CALL-shaped ops, fill its reply promise. Calls the ctl_* impls below.
    void dispatch(ExecCommand& cmd);
    // Subtree traversal.
    std::vector<WorkerNode*> all_workers(SupervisorNode& sup);
    SupervisorNode* supervisor_of(WorkerNode& w);

    // Start / stop primitives.
    void start_worker(WorkerNode& w);
    void start_subtree(SupervisorNode& sup);
    void stop_worker(WorkerNode& w);
    void shutdown_subtree(SupervisorNode& sup);
    // Two-phase group stop: SIGTERM EVERY worker first (no wait), then reap them
    // all against ONE shared deadline. Collapses an N-child shutdown from
    // N×timeout (sequential stop_worker) to ~1×timeout. `workers` must already
    // be in stop order (dependents first); the signal pass preserves it.
    void signal_worker(WorkerNode& w);   // SIGTERM/SIGKILL, mark terminating, no wait
    void reap_worker(WorkerNode& w,
                     std::chrono::steady_clock::time_point deadline);  // wait + SIGKILL straggler
    void stop_workers(const std::vector<WorkerNode*>& workers);

    // Restart strategy dispatch.
    void on_child_exit(WorkerNode& w, int return_code, pid_t old_pid);
    bool record_and_check_restart(SupervisorNode& sup);
    // Cumulative restart stat: bump the worker's lifetime counter AND its
    // supervisor's, in lockstep, so the snapshot's "restarts" column means the
    // same thing (total restarts) on a process row and a supervisor row.
    void bump_restart_count_(WorkerNode& w, SupervisorNode& sup);
    void restart_all(SupervisorNode& sup);
    void restart_rest(SupervisorNode& sup, WorkerNode& failed);

    // Reap any exited workers (non-blocking).
    void reap();

    std::unique_ptr<Node>            root_node_;
    SupervisorNode*                  root_;
    // Immutable name → TIPC address index, built once from root_node_ in the
    // ctor. Read-only thereafter; resolve() is lock-free and safe from any
    // thread (the manifest topology never changes). See registry.h.
    Registry                         registry_;
    std::string                      root_dir_;
    std::atomic<bool>                shutdown_requested_{false};
    bool                             escalated_{false};

    // signalfd descriptor and a self-pipe wake-up fd for portability.
    int                              signal_fd_{-1};

    // Command queue + its eventfd wake. enqueue()/call() (any thread) push a
    // typed ExecCommand and write the eventfd; the select() loop adds the
    // eventfd to its fd_set, drains it, and dispatches each command on the loop
    // thread. cmd_eventfd_ is EFD_NONBLOCK | EFD_CLOEXEC; a single counter.
    // cmd_mutex_ guards ONLY this queue (the actor ingress) — there is NO lock
    // on the tree/config state: the loop thread is the single writer.
    int                              cmd_eventfd_{-1};
    std::mutex                       cmd_mutex_;
    std::deque<ExecCommand>          cmd_queue_;

    // The select-loop thread id, set at the top of run(). call() compares
    // against it: a call() issued FROM the loop thread dispatches INLINE instead
    // of enqueue+block, which would self-deadlock (only the loop fulfils the
    // promise). No current caller does this — resolve now reads the lock-free
    // Registry — but the guard keeps a future loop-thread call() safe. Off-loop
    // callers (SupervisorCtl's TipcMux thread) take the normal enqueue+future.
    std::thread::id                  loop_tid_{};

    // Outbound emit surface — installed by the FC shell via set_emit_sink();
    // forwards events/health/topo-pairs to SupervisorCtl's broadcast senders.
    // Replaces the legacy TipcPublisher + EtcdPublisher members entirely.
    EmitSink                         emit_;

    std::chrono::steady_clock::time_point start_time_{};
    std::chrono::steady_clock::time_point last_heartbeat_{};
    std::chrono::steady_clock::time_point last_snapshot_{};
    uint64_t                         generation_{0};
    uint64_t                         total_restarts_{0};
    uint64_t                         total_tombstones_{0};

    void emit_event(uint32_t kind, const WorkerNode* worker,
                    const SupervisorNode* sup, int exit_code,
                    const std::string& tombstone_path,
                    const std::string& detail);
    void emit_health();
    HealthData compute_health();     // shared by emit_health + ctl_get_health
    void ctl_get_health(ExecReply& rep);
    void emit_snapshot();

    // #429/#430 — the topo-pair firehose stream over the EmitSink.
    // emit_tree_stream() does a FULL walk: SnapshotBegin →
    // {NodeEdge(ADD)+NodeState} topo-ordered → SnapshotEnd via emit_.on_*.
    // cast_node_state() sends one NodeState (emit_.on_node_state) for an
    // incremental single-node change (restart/coredump/degraded).
    void emit_tree_stream();
    void cast_node_state(const WorkerNode& w);

    // ---- Control-op primitives (called from the ctl_* wrappers) ------------
    // Each looks up children by name in the current tree and mutates as the
    // OTP semantics require, returning a status ordinal. No envelope: the
    // SupervisorCtl gen_server dispatches one typed CALL per op.
    WorkerNode*     find_worker_by_name(const std::string& name);
    SupervisorNode* find_supervisor_by_name(const std::string& name);

    // OTP start_child / delete_child: hot-add and hot-remove. Both
    // mutate the parent supervisor's children vector in place.
    // start_child returns the newly-spawned pid (or -1 if start_cmd
    // is missing / fork failed).
    // delete_child fails with status=running if the spec is still
    // running; the caller must terminate_child first.
    pid_t        do_start_child(const std::string& parent_sup,
                                const std::string& name,
                                const std::vector<std::string>& start_cmd,
                                int restart, int shutdown, int type,
                                const std::vector<std::string>& modules,
                                /*out*/ uint32_t& status);
    uint32_t     do_delete_child(const std::string& name);

    // RestartChild = stop then start. TerminateChild = stop with the
    // spec retained for a later RestartChild.
    uint32_t     do_restart_child(const std::string& name);
    uint32_t     do_terminate_child(const std::string& name);

    // SuspendChild / ResumeChild (test harness). Suspend stops + HOLDS the
    // child down (no restart, no watchdog escalation) so a probe can mock its
    // node; Resume clears the hold and restarts. See WorkerNode::held.
    uint32_t     do_suspend_child(const std::string& name);
    uint32_t     do_resume_child(const std::string& name);

    // GetSystemInfo — populate a SystemInfoData with host facts on demand.
    // Best-effort; missing fields stay at struct defaults. Cheap
    // (uname + a few /proc + /etc reads); not cached.
    void         do_get_system_info(SystemInfoData& info_out);

    // /proc/<pid>/{stat,status} sampler — one row per supervised pid.
    // Refreshed inside the main loop on the heartbeat tick and read by
    // emit_snapshot. Resource facts ride on each ChildState wire row
    // so the GUI never reads /proc itself.
    // One thread's snapshot — mirrors the .art ThreadSample message
    // 1:1 so emit_snapshot can copy fields straight into the protobuf.
    struct ThreadEntry {
        uint32_t  tid{0};
        std::string comm;
        uint32_t  cpu_pct{0};
        uint32_t  sched_policy{0};
        uint32_t  sched_priority{0};
        int32_t   nice{0};
        uint64_t  cpu_affinity_mask{0};
        uint32_t  last_cpu{0};
        // Delta state: previous (utime+stime) jiffies for this tid.
        uint64_t  prev_jiffies{0};
    };

    struct ProcSample {
        uint32_t cpu_pct{0};           // SMOOTHED cpu, hundredths of a percent
        uint64_t rss_kb{0};
        uint64_t vsz_kb{0};
        uint64_t shared_kb{0};         // Shared_Clean+Shared_Dirty (smaps_rollup)
        uint64_t data_kb{0};           // VmData — heap+bss+data
        uint32_t threads{0};
        // Previous (utime + stime) in jiffies — used to compute the
        // delta CPU% each tick.
        uint64_t prev_jiffies{0};
        // Smoothed CPU as a double (% — NOT hundredths), EWMA of the raw
        // per-tick %. The raw value is jiffy-quantized: at clk_tck=100, one
        // jiffy ≈ 1% of a 1s window, so a near-idle process reads a 0↔1%
        // sawtooth. The EWMA turns that into a steady low value (e.g. ~0.2%).
        // `cpu_ewma_init` guards the first sample (seed, don't blend from 0).
        double   cpu_ewma{0.0};
        bool     cpu_ewma_init{false};

        // Per-thread breakdown, keyed by tid so delta computation
        // survives across ticks.
        std::map<uint32_t, ThreadEntry> threads_detail;
        // TIPC sockets owned by this pid this tick (queue depths + addrs).
        std::vector<TreeSocketRow> sockets;
    };
    void sample_procs();               // refresh sample_ for all live pids

    std::map<pid_t, ProcSample>      sample_;
    std::chrono::steady_clock::time_point last_proc_sample_{};
    // THEIA_TRACE_CPU=1 → log the raw /proc cpu inputs per tick so a 0%% cpu_pct
    // reading is explainable (idle / fresh pid / no interval / read fail).
    const bool cpu_trace_ = (std::getenv("THEIA_TRACE_CPU") != nullptr);

    // Watchdog state (phase 4): last heartbeat timestamp per pid + the
    // last seq we saw. ``check_heartbeats()`` runs every tick of the
    // main loop; pids that have missed `kHeartbeatMaxAge` get SIGTERMed
    // and the restart strategy kicks in via the normal on_child_exit
    // path. We only watchdog pids that have ever delivered a beat —
    // children configured without a HeartbeatPublisher (most demo /
    // bash daemons) are exempt.
    struct HeartbeatState {
        std::chrono::steady_clock::time_point last_seen;
        uint64_t                              last_seq{0};
    };
    std::map<pid_t, HeartbeatState>  heartbeats_;
    void check_heartbeats();

    // ---- process groups (pg) — OTP-style broadcast fan-out ------------------
    //
    // A group is keyed by group_id = the wire message-type service_id (the same
    // djb2 register_cast demuxes on). pg_groups_[group_id][pid] = the member's
    // RECEIVE address. pg_watchers_[group_id][pid] = a broadcaster of that type
    // (so we know whom to push membership to). Both are reaped by the watchdog:
    // pg_reap_pid() drops a dead pid from every group + watcher set and re-pushes
    // membership to affected groups — liveness is the heartbeat, NOT a per-member
    // socket. This makes pg available to ANY heartbeating node, not just children.
    struct PgMemberRec { uint32_t tipc_type{0}; uint32_t tipc_instance{0}; };
    std::map<uint32_t, std::map<pid_t, PgMemberRec>> pg_groups_;    // group→members
    std::map<uint32_t, std::map<pid_t, PgMemberRec>> pg_watchers_;  // group→broadcasters

    void ctl_pg_join(const std::string& node, pid_t pid, uint32_t group_id,
                     uint32_t tipc_type, uint32_t tipc_instance);
    void ctl_pg_leave(pid_t pid, uint32_t group_id);
    void ctl_pg_watch(const std::string& node, pid_t pid, uint32_t group_id,
                      uint32_t tipc_type, uint32_t tipc_instance);
    void pg_reap_pid(pid_t pid);                 // drop pid from all groups (watchdog)
    void push_pg_membership(uint32_t group_id);  // push to every watcher of group_id
    bool pg_has_pid(pid_t pid) const;            // is pid in any group/watcher set?

    // ---- Trace config (#361, #403) -----------------------------------------
    //
    // The supervisor receives ConfigureTrace from tdb/services-com, remembers
    // which trace KINDS each child has enabled, and pushes them to the node's
    // config service. Keyed by child NAME (the worker process), not pid, so it
    // survives restart: the first HeartbeatReport after a gap fires a re-push.
    //
    // A node's Tracer kind filter is a BITMASK — several kinds can be on at once
    // (CAST_IN | CALL_OUT | ...). So we store a SET of enabled TraceKind
    // ordinals per child (the inner map is keyed by KIND; presence = that kind
    // is enabled). The catch-all "all kinds" push (kind 0 / Other) is stored as
    // the single kind 0. Empty (no entries) means "no trace for that child" —
    // no push is sent. Read-back lists every enabled kind; restart re-push casts
    // one TraceControlPush per enabled kind so the node rebuilds its mask.
    std::map<std::string,
             std::map<uint32_t, bool>>  trace_configs_;

    // Tracks last heartbeat time per child NAME (not pid) — so we can
    // detect a "fresh start after gap" trigger independent of pid.
    // Updated in the kTagHeartbeat handler alongside heartbeats_[pid].
    std::map<std::string,
             std::chrono::steady_clock::time_point>
                                                 last_heartbeat_by_child_;

    // Deferred config re-apply on (re)start. start_worker() arms a
    // per-child deadline; once it elapses (child has had a grace window to
    // bind its config-service TIPC socket) the main loop re-pushes the
    // stored trace config + log level. This is what makes config survive a
    // CRASH for FCs that don't send heartbeats (the heartbeat-after-gap
    // path only fires for reporting nodes that beat). Crash-investigation:
    // the trace you armed is re-applied to the freshly-restarted child.
    std::map<std::string,
             std::chrono::steady_clock::time_point>
                                                 config_repush_due_;

    // Apply one TraceConfig: update the in-memory table and push to
    // the node (best-effort — failure to push is logged, not fatal).
    // Set/clear ONE trace kind for a child. kind 0 (Other / catch-all) when
    // enabled REPLACES the set with just the catch-all (the node wipes its mask
    // → all kinds pass); a non-zero kind ADDS to the set. Disabling kind 0 (or
    // any kind that empties the set) clears the child's trace.
    void apply_trace_config(const std::string& target_node,
                            uint32_t kind,
                            bool enabled);

    // Push the WHOLE trace_configs_[child] map to that child's
    // NodeTraceCtl TIPC server. Called on (re)start of the worker and
    // on the first heartbeat after a gap. The peer's TIPC address is
    // looked up from the worker's NodeInfo.tipc_{type,instance}.
    // No-op when the child has no entries.
    void push_trace_config_to_child(const std::string& child_name);

    // Per-child log level (#385). tdb → services/com → here.
    // Same survives-restart contract as trace: store the level,
    // overwrite the child's spawn env THEIA_LOG_LEVEL so a (re)start
    // boots at the new level, and push a LogLevelConfig frame to the
    // child's node for live (no-restart) application. log_levels_ is
    // keyed by child NAME so it persists across pid changes. Stores the
    // LogLevelValue ORDINAL (0..4) the wire carried — forwarded verbatim to
    // the live push; mapped to a name only for the spawn env. Sentinel kNoLevel
    // = "no override, use the manifest env default".
    static constexpr uint32_t kNoLevel = 0xFFFFFFFFu;
    std::map<std::string, uint32_t>              log_levels_;

    // Apply one LogLevelConfig: store the ordinal, update the worker's env map
    // (so a restart re-applies), and push live. Best-effort push.
    void apply_log_level(const std::string& target_node, uint32_t level);

    // Push the stored log level to a child's node TIPC server, the
    // same way push_trace_config_to_child does. Called on apply and
    // on the first heartbeat after a gap (re-push). No-op when the
    // child has no stored level.
    void push_log_level_to_child(const std::string& child_name);
};

}  // namespace supervisor

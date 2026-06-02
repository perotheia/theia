// The Supervisor ENGINE. fork/exec, signal handling, restart logic.
//
// Transport-FREE: this class owns the supervision state (the child tree,
// the watchdog table, the restart strategy) and the select()-loop that
// drives it. It no longer binds any TIPC socket itself. The gen-app FC
// shell wraps it:
//   - SupervisorWorker (runnable) constructs the process-global Supervisor
//     and runs do_loop() == run() (the select loop, sole state owner).
//   - SupervisorCtl (atomic gen_server) receives the control surface over
//     the STANDARD Theia transport (nanopb on TipcMux) and post_command()s
//     each op into this engine on the loop thread.
// Outbound events/health/topo-pairs leave via the EmitSink callbacks below,
// which the FC shell wires to SupervisorCtl's `events` broadcast senders.
// Per-node trace/log config + the SM startup handshake still cast raw
// GW_MSG_GEN_CAST frames straight to the target's TIPC name (see runtime.cpp).

#pragma once

#include "spec.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>
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
struct SystemInfoData {
    std::string hostname;
    std::string kernel;
    std::string os_pretty_name;
    uint32_t    cpu_count{0};
    uint64_t    total_ram_kb{0};
    uint64_t    uptime_sec{0};
    std::string theia_git_sha;
    std::string build_timestamp;
    uint64_t    start_timestamp_ms{0};
};

// One read-back trace-config row for GetTraceConfig.
struct TraceConfigRow {
    std::string target_node;
    std::string msg_type;
    uint32_t    kind{0};   // TraceKind ordinal; enabled is implicit (present)
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

    // Drive the tree. Returns the process exit code. This IS
    // SupervisorWorker::do_loop(): the signalfd + command-queue select loop,
    // sole owner of all supervision state.
    int run();

    // Request graceful shutdown — safe to call from a signal handler or the
    // FC shell's do_stop().
    void request_shutdown();

    // #431 — command queue (the threading bridge). SupervisorCtl::handle_call
    // runs on the TipcMux thread; the select() loop is the SOLE owner of all
    // supervision state (reap/sample/emit/fork). handle_call builds a closure
    // that mutates the engine + fulfils its own std::promise, then
    // post_command()s it and wakes the loop. The loop drains + runs every
    // queued closure inline — same thread as reap() — so there is ONE writer,
    // no mutex around the tree, no fork-under-lock. Callable from any thread.
    void post_command(std::function<void()> fn);

    // ---- Control-surface primitives (called from SupervisorCtl::handle_call
    //      via post_command, so they run on the loop thread). All take/return
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

    // ConfigureTrace / ConfigureLogLevel — store + push (survives restart).
    void ctl_configure_trace(const std::string& target_node,
                             const std::string& msg_type,
                             bool enabled, uint32_t kind);
    void ctl_configure_log_level(const std::string& target_node,
                                 const std::string& level);

    // GetTraceConfig read-back: flatten the per-child trace_configs_ table.
    std::vector<TraceConfigRow> ctl_get_trace_config();

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
    // Drain + run all queued commands on the loop thread. Called once per
    // select() iteration BEFORE reap/sample/emit.
    void drain_commands();
    // Subtree traversal.
    std::vector<WorkerNode*> all_workers(SupervisorNode& sup);
    SupervisorNode* supervisor_of(WorkerNode& w);

    // Start / stop primitives.
    void start_worker(WorkerNode& w);
    void start_subtree(SupervisorNode& sup);
    void stop_worker(WorkerNode& w);
    void shutdown_subtree(SupervisorNode& sup);

    // Restart strategy dispatch.
    void on_child_exit(WorkerNode& w, int return_code, pid_t old_pid);
    bool record_and_check_restart(SupervisorNode& sup);
    void restart_all(SupervisorNode& sup);
    void restart_rest(SupervisorNode& sup, WorkerNode& failed);

    // Reap any exited workers (non-blocking).
    void reap();

    std::unique_ptr<Node>            root_node_;
    SupervisorNode*                  root_;
    std::string                      root_dir_;
    std::atomic<bool>                shutdown_requested_{false};
    bool                             escalated_{false};

    // signalfd descriptor and a self-pipe wake-up fd for portability.
    int                              signal_fd_{-1};

    // #431 — command queue + its eventfd wake. post_command() (any thread)
    // pushes a closure and writes the eventfd; the select() loop adds the
    // eventfd to its fd_set, drains it, and runs the closures on the loop
    // thread. cmd_eventfd_ is EFD_NONBLOCK | EFD_CLOEXEC; a single counter.
    int                              cmd_eventfd_{-1};
    std::mutex                       cmd_mutex_;
    std::deque<std::function<void()>> cmd_queue_;

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
    // Resolve a node-type name to {hosting worker, that node's NodeInfo}.
    std::pair<WorkerNode*, const NodeInfo*>
                    find_node_by_name(const std::string& name);
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
        uint32_t cpu_pct{0};           // hundredths of a percent
        uint64_t rss_kb{0};
        uint64_t vsz_kb{0};
        uint64_t shared_kb{0};         // Shared_Clean+Shared_Dirty (smaps_rollup)
        uint64_t data_kb{0};           // VmData — heap+bss+data
        uint32_t threads{0};
        // Previous (utime + stime) in jiffies — used to compute the
        // delta CPU% each tick.
        uint64_t prev_jiffies{0};

        // Per-thread breakdown, keyed by tid so delta computation
        // survives across ticks.
        std::map<uint32_t, ThreadEntry> threads_detail;
    };
    void sample_procs();               // refresh sample_ for all live pids

    std::map<pid_t, ProcSample>      sample_;
    std::chrono::steady_clock::time_point last_proc_sample_{};

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

    // ---- Trace config (#361) -----------------------------------------------
    //
    // The supervisor receives ConfigureTrace from supdbg/services-com,
    // remembers the (target_node, msg_type) → enabled tuple, and pushes
    // it to the node's NodeTraceCtl TIPC server. The config is keyed by
    // child NAME (the worker process), not pid, so it survives restart:
    // the first HeartbeatReport after a gap fires a re-push.
    //
    // Storage: configs_by_child[child_name][msg_type] = TraceKind ordinal.
    // Presence of the (child, msg_type) key means "enabled"; the value is
    // the trace KIND to push (#403; 0 = all kinds). Empty (no entries)
    // means "no trace for that child" — no push is sent.
    std::map<std::string,
             std::map<std::string, uint32_t>>  trace_configs_;

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
    void apply_trace_config(const std::string& target_node,
                            const std::string& msg_type,
                            bool enabled,
                            uint32_t kind = 0);

    // Push the WHOLE trace_configs_[child] map to that child's
    // NodeTraceCtl TIPC server. Called on (re)start of the worker and
    // on the first heartbeat after a gap. The peer's TIPC address is
    // looked up from the worker's NodeInfo.tipc_{type,instance}.
    // No-op when the child has no entries.
    void push_trace_config_to_child(const std::string& child_name);
    // Push an explicit TraceControlPush{enabled=false} (disable) to the node —
    // the config is already erased, so push_trace_config_to_child won't.
    void push_trace_disable_to_child(const std::string& child_name);
    // Resolve a trace target name (worker OR node-type) to its TIPC addr.
    bool resolve_trace_target(const std::string& child_name,
                              uint32_t& type, uint32_t& instance);

    // Per-child log level (#385). supdbg → services/com → here.
    // Same survives-restart contract as trace: store the level,
    // overwrite the child's spawn env THEIA_LOG_LEVEL so a (re)start
    // boots at the new level, and push a LogLevelConfig frame to the
    // child's node for live (no-restart) application. log_levels_ is
    // keyed by child NAME so it persists across pid changes; an empty
    // value means "use the default from the manifest env".
    std::map<std::string, std::string>           log_levels_;

    // Apply one LogLevelConfig: store it, update the worker's env map
    // (so a restart re-applies), and push live. Best-effort push.
    void apply_log_level(const std::string& target_node,
                         const std::string& level);

    // Push the stored log level to a child's node TIPC server, the
    // same way push_trace_config_to_child does. Called on apply and
    // on the first heartbeat after a gap (re-push). No-op when the
    // child has no stored level.
    void push_log_level_to_child(const std::string& child_name);
};

}  // namespace supervisor

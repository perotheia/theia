// The Supervisor class. fork/exec, signal handling, restart logic.
//
// Transport: the supervisor exposes itself as a TIPC service at the
// address declared in platform/supervisor/system/package.art (TIPC
// type 0x80020001 / instance 0). Same SOCK_SEQPACKET framing every
// other artheia node uses. services/com bridges this to gRPC for the
// external GUI; in-host actors talk to it directly.

#pragma once

#include "supervisor/spec.h"
#include "supervisor/tipc_publisher.h"
#include "supervisor/etcd_publisher.h"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

namespace supervisor {

class Supervisor {
public:
    // etcd_endpoints: comma-separated "host:port" list. Empty disables
    // the etcd-side state publisher (the TIPC publisher always runs).
    // machine_name: identifies this supervisor's keys under /theia/.
    //   Defaults to gethostname() if empty.
    Supervisor(std::unique_ptr<Node> root,
                std::string root_dir,
                std::string etcd_endpoints = "",
                std::string machine_name   = "");
    ~Supervisor();

    Supervisor(const Supervisor&)            = delete;
    Supervisor& operator=(const Supervisor&) = delete;

    // Drive the tree. Returns the process exit code.
    int run();

    // Request graceful shutdown — safe to call from a signal handler.
    void request_shutdown() { shutdown_requested_.store(true); }

private:
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

    // TIPC fan-out at the .art-declared address. Hosts both the
    // outbound publish path (events / health / snapshots) and the
    // inbound dispatch hook for ControlRequest (phase 3) /
    // HeartbeatReport / SendTimeoutReport (phase 4).
    TipcPublisher                    publisher_;

    // etcd-side state publisher. Tees the same state TIPC carries
    // into /theia/state/<machine>/* and /theia/events/<machine>/*
    // for the supervisor-gui Table Viewer + observer-style tabs
    // that consume etcd. Opt-in via etcd_endpoints; no-op when
    // disabled. See supervisor/etcd_publisher.h for details.
    EtcdPublisher                    etcd_publisher_;

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

    // Inbound TIPC dispatch — called by the publisher's poll() when
    // a client frame lands. Routes by tag into the dedicated handlers
    // below; replies via publisher_.reply_to(client_fd, ...).
    void on_inbound_frame(int client_fd, uint16_t tag,
                          const std::string& payload);

    // ---- ControlRequest handlers (phase 3) ---------------------------------
    // Each returns a populated ControlReply payload (correlation_id is
    // filled by the caller). All look up children by name in the
    // current tree and mutate as the OTP semantics require.
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
};

}  // namespace supervisor

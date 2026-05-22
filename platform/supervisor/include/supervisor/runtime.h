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
    Supervisor(std::unique_ptr<Node> root, std::string root_dir);
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
    // a client frame lands. Phase 3 fills in ControlRequest handling
    // (StartChild / RestartChild / TerminateChild / DeleteChild),
    // phase 4 fills HeartbeatReport / SendTimeoutReport.
    void on_inbound_frame(int client_fd, uint16_t tag,
                          const std::string& payload);

    // /proc/<pid>/{stat,status} sampler — one row per supervised pid.
    // Refreshed inside the main loop on the heartbeat tick and read by
    // emit_snapshot. Resource facts ride on each ChildState wire row
    // so the GUI never reads /proc itself.
    struct ProcSample {
        uint32_t cpu_pct{0};           // hundredths of a percent
        uint64_t rss_kb{0};
        uint64_t vsz_kb{0};
        uint32_t threads{0};
        // Previous (utime + stime) in jiffies — used to compute the
        // delta CPU% each tick.
        uint64_t prev_jiffies{0};
    };
    void sample_procs();               // refresh sample_ for all live pids

    std::map<pid_t, ProcSample>      sample_;
    std::chrono::steady_clock::time_point last_proc_sample_{};
};

}  // namespace supervisor

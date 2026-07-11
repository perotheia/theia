// Erlang-style supervisor spec (data shapes).
//
// Mirrors armanifest/supervisor.py — the YAML format is the
// contract between the Python tooling and this binary.
//
// References:
//   https://erlang.org/documentation/doc-4.9.1/doc/design_principles/sup_princ.html
//   docs/supervision.md

#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace supervisor {

enum class RestartStrategy {
    OneForOne,
    OneForAll,
    RestForOne,
    SimpleOneForOne,  // not implemented at runtime; logged
};

enum class RestartType {
    Permanent,
    Transient,
    Temporary,
};

// Shutdown is either:
//   - milliseconds (>= 0) — SIGTERM then SIGKILL after timeout
//   - kBrutalKill           — immediate SIGKILL, no SIGTERM
//   - kInfinity             — wait forever after SIGTERM
struct Shutdown {
    enum Kind { kTimeout, kBrutalKill, kInfinity };
    Kind kind{kTimeout};
    int  timeout_ms{5000};

    static Shutdown timeout(int ms)   { return {kTimeout, ms}; }
    static Shutdown brutal_kill()     { return {kBrutalKill, 0}; }
    static Shutdown infinity()        { return {kInfinity, 0}; }
};

// Forward decl so SupervisorNode can hold children of mixed kind.
struct Node;

// Per-art-node metadata for a worker process (#366, #364).
//
// Each `node atomic <Name>` in the FC's .art becomes a runtime thread
// inside the worker process. The executor.yaml `nodes:` block lists
// them; the supervisor uses the list two ways:
//
//   (1) Watchdog: HeartbeatReport.node_name is matched against this
//       list so a missing heartbeat from a specific thread can be
//       attributed to a specific .art node.
//   (2) Snapshot synthesis: a worker with ≥1 reporting=true node
//       gets a synthetic `<worker>_sup [one_for_all]` row inserted
//       into TreeSnapshot between the worker and its parent
//       (#364). The GUI right-clicks node rows under this _sup row
//       to drive ConfigureTrace (#365).
struct NodeInfo {
    std::string name;
    bool        reporting{true};
    std::string tipc_type;      // hex string like "0x80010001"
    std::string tipc_instance;  // decimal string like "0"

    // Per-node CPU affinity + scheduler (from the rig's NodeToCPUMapping,
    // executor.json nodes[]). The supervisor serializes these into the child's
    // THEIA_NODE_CFG env; the hosting process's main.cc applies them to the
    // node's thread (apply_node_affinity). Empty/default = leave node unpinned.
    std::vector<int> cpus;          // affinity set; empty = inherit process
    std::string      sched;         // "other"|"batch"|"idle"|"fifo"|"rr"|"deadline"|""
    int              sched_prio{0}; // rtprio for fifo/rr
};

struct WorkerNode {
    std::string                          name;
    std::vector<std::string>             start_cmd;
    RestartType                          restart{RestartType::Permanent};
    Shutdown                             shutdown{Shutdown::timeout(5000)};
    std::vector<std::string>             modules;
    std::map<std::string, std::string>   env;
    std::string                          working_dir;
    std::vector<NodeInfo>                nodes;  // per-art-node metadata

    // run_on_start (executor.json, default true). When false, the worker is
    // DEFINED in the tree (so the supervisor knows it + a probe/operator can
    // start it later via ctl_start_child) but is NOT started at boot. For
    // HW-dependent FCs that must not auto-touch the environment in a given deploy
    // — e.g. nm on a container/CI rig, where it would reconfigure the host's
    // SSH-bearing interface. Distinct from `held` (a runtime SuspendChild) and
    // from RestartType (post-exit behavior): this gates the BOOT start only.
    bool                                 run_on_start{true};

    // CPU affinity, AUTOSAR-flavoured (ProcessToMachineMapping in §9.4).
    // Mutually exclusive: either shall_run_on or shall_not_run_on, not
    // both. Empty lists mean "no affinity constraint".
    //   shall_run_on:     positive list of core IDs the process may use.
    //   shall_not_run_on: cores the process must avoid; the supervisor
    //                     resolves to a positive mask against the host's
    //                     online CPUs.
    std::vector<int>                     shall_run_on;
    std::vector<int>                     shall_not_run_on;

    // Per-child PROCESS memory cap (bytes). Applied as RLIMIT_AS in the fork
    // before execvp — the whole child process (all its node threads) shares it,
    // since RLIMIT is per-process not per-thread. 0 = no cap. From the rig's
    // Process.mem_limit (manifest), carried in executor.json as mem_limit_bytes.
    uint64_t                             mem_limit_bytes{0};

    // Runtime state.
    int                                  pid{-1};      // -1 when not running
    std::chrono::steady_clock::time_point last_start{};

    // #429 — per-node status carried on NodeState.flags / ChildState.flags.
    // Bitmask: bit0 CORE_DUMPED (last exit dumped core), bit1 DEGRADED
    // (restart budget nearing exhaustion). Cleared on a clean (re)start.
    uint32_t                             flags{0};
    // Cumulative restart count + last observed exit code, for NodeState.
    uint32_t                             restart_count{0};
    int                                  last_exit_code{0};
    // True while stop_worker() is actively terminating this worker. When
    // set, reap() skips on_child_exit() for this PID — the synchronous
    // stop path owns the wait. Mirrors OTP's {restarting, OldPid} sentinel
    // (supervisor.erl line ~525, ~1537), which prevents an in-flight
    // shutdown's SIGCHLD from triggering a spurious restart.
    bool                                 terminating{false};

    // Test-harness HOLD (SuspendChild). When set, the worker is stopped and
    // kept DOWN: reap() skips on_child_exit() (no restart strategy) and the
    // watchdog skips this worker (no missed-heartbeat SIGTERM/escalation), so
    // a probe can take over its TIPC address to mock the node. ResumeChild
    // clears it and restarts. Distinct from `terminating` (a transient
    // in-flight stop); `held` persists until an explicit resume.
    bool                                 held{false};
};

struct SupervisorNode {
    std::string                          name;
    RestartStrategy                      strategy{RestartStrategy::OneForOne};
    int                                  max_restarts{3};
    int                                  max_seconds{5};
    std::vector<std::unique_ptr<Node>>   children;

    // Sliding-window of restart timestamps (steady_clock) — the OTP restart-
    // INTENSITY check (max_restarts in max_seconds). Decays as entries age out;
    // an INTERNAL supervision signal, NOT the user-facing stat.
    std::vector<std::chrono::steady_clock::time_point> restart_history;

    // Cumulative, monotonic count of child restarts under this supervisor —
    // the user-facing "restarts" stat (matches WorkerNode::restart_count's
    // lifetime semantics, so the GUI/tdb column means the same thing on a
    // supervisor row as on a process row). Bumped alongside a child's
    // restart_count; never decays. (Distinct from restart_history, which is
    // the windowed intensity check.)
    uint32_t                             restart_count{0};

    // Back-pointer; not owning. nullptr at root.
    SupervisorNode* parent{nullptr};

    // Project extension (root-only): where to look for tombstones when
    // a child dies from a fatal signal. Leaf supervisors inherit by
    // walking up to the root. Empty == no tombstone surfacing.
    std::string tombstone_dir;
};

// Tagged-union: a tree node is either a worker (leaf) or a supervisor
// (internal). Using unique_ptr<Node> + variant-like tag because we need
// stable addresses when handing pointers around the runtime.
struct Node {
    enum Kind { kWorker, kSupervisor };
    Kind                                 kind;
    WorkerNode                           worker;
    SupervisorNode                       sup;

    static std::unique_ptr<Node> make_worker(WorkerNode w) {
        auto n = std::unique_ptr<Node>(new Node);
        n->kind = kWorker;
        n->worker = std::move(w);
        return n;
    }
    static std::unique_ptr<Node> make_supervisor(SupervisorNode s) {
        auto n = std::unique_ptr<Node>(new Node);
        n->kind = kSupervisor;
        n->sup = std::move(s);
        return n;
    }

    bool is_worker()      const { return kind == kWorker; }
    bool is_supervisor()  const { return kind == kSupervisor; }
};

// JSON loader. Throws std::runtime_error on malformed input. *machine_out (when
// non-null) receives the root's optional "machine" name (the board this manifest
// is for); "" if absent.
std::unique_ptr<Node> load_manifest(const std::string& path,
                                    std::string* machine_out = nullptr);

// String <-> enum helpers (used by both loader and logging).
RestartStrategy parse_strategy(const std::string& s);
RestartType     parse_restart_type(const std::string& s);
const char*     to_string(RestartStrategy s);

}  // namespace supervisor

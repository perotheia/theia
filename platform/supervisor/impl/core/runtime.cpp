// Supervisor ENGINE — fork/exec children, observe exits, apply OTP-style
// restart strategy. Transport-free: it owns the supervision state + the
// select() loop; the gen-app FC shell (SupervisorWorker + SupervisorCtl)
// wraps it. Mirrors supervisor/runtime.py.
//
// No protobuf in this translation unit, and NO transport. Outbound
// events/health/topo-pairs AND the per-node trace/log config push all leave via
// the EmitSink callbacks (on_event/on_health/on_edge/on_node_state/
// on_config_push), wired by the FC shell to SupervisorCtl's `events` broadcast
// senders and (for config push) to the SupervisorWorker runnable's runtime
// TheiaMsgHeader cast. The nanopb<->engine translation for inbound control ops
// lives in SupervisorCtl_handlers.cc. The engine resolves a child's TIPC
// (type,instance) + hand-encodes the proto3 payload, but never opens a socket
// itself (the old hand-rolled GwHdrWire send_gw_cast_to_tipc_name is gone).

#include "runtime.h"

#include "Logger.hh"   // platform/runtime — process_logger()

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/utsname.h>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <sched.h>
#include <sys/prctl.h>      // PR_CAP_AMBIENT_RAISE — pass CAP_SYS_NICE to children
#include <sys/syscall.h>    // SYS_capget / SYS_capset (raw, no libcap dep)
#include <linux/capability.h>  // CAP_SYS_NICE + cap header/data structs
// TIPC per-socket queue depths come from `ss --tipc -e` (popen) — the kernel's
// TIPC sock_diag returns TIPC-specific netlink attrs that ss already parses
// (incl. the fs inode that matches /proc/<pid>/fd).
#include <sys/resource.h>   // setrlimit(RLIMIT_AS) — per-process memory cap
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>    // statvfs — disk free for SystemInfo (System tab)
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

namespace supervisor {

namespace {

// The engine routes ALL its lines through the node logger SupervisorWorker
// injects via set_logger() (a ContextLogger tagged [#supervisor_worker]) — set
// before run(), so it's live for every log_* call. Until set (or in a unit
// harness), it falls back to the process logger. File-local so the free helpers
// below keep their bare-name call sites; Supervisor::set_logger points it here.
::theia::runtime::Logger* g_engine_logger = nullptr;

::theia::runtime::Logger& engine_log() {
    return g_engine_logger ? *g_engine_logger
                           : ::theia::runtime::process_logger();
}

void log_debug(const std::string& m) { engine_log().debug(m); }
void log_info(const std::string& m)  { engine_log().info(m); }
void log_warn(const std::string& m)  { engine_log().warn(m); }
void log_err (const std::string& m)  { engine_log().error(m); }

std::string join(const std::vector<std::string>& xs, char sep = ' ') {
    std::string out;
    for (size_t i = 0; i < xs.size(); ++i) {
        if (i) out += sep;
        out += xs[i];
    }
    return out;
}

bool path_is_absolute(const std::string& p) {
    return !p.empty() && p[0] == '/';
}

// Walk up from `sup` to find the nearest non-empty tombstone_dir.
std::string find_tombstone_dir(const supervisor::SupervisorNode* sup) {
    for (auto* n = sup; n != nullptr; n = n->parent) {
        if (!n->tombstone_dir.empty()) return n->tombstone_dir;
    }
    return {};
}

uint64_t epoch_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

// Locate the most recent tombstone matching <dir>/tombstone-<name>-<prefix>*.
// `prefix` is "<name>-<pid>-" for an exact-pid match (the crash-time surfacing)
// or "<name>-" to find the newest tombstone for a child across ANY pid (the
// GUI fetch, where the child has since restarted with a new pid). Newest mtime
// wins. Returns "" if no match.
std::string locate_tombstone_prefixed(const std::string& dir,
                                      const std::string& prefix_body) {
    DIR* d = opendir(dir.c_str());
    if (!d) return {};
    std::string prefix = "tombstone-" + prefix_body;
    std::string best;
    time_t best_mt = 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string fn = e->d_name;
        if (fn.compare(0, prefix.size(), prefix) != 0) continue;
        std::string full = dir + "/" + fn;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && st.st_mtime >= best_mt) {
            best = std::move(full);
            best_mt = st.st_mtime;
        }
    }
    closedir(d);
    return best;
}

// Crash-time surfacing: exact <name>-<pid> match.
std::string locate_tombstone(const std::string& dir,
                             const std::string& name,
                             pid_t pid) {
    return locate_tombstone_prefixed(dir, name + "-" + std::to_string(pid) + "-");
}

}  // namespace

namespace {
// Validate the root tree BEFORE the init list dereferences it for the Registry.
// Throws on a null / non-supervisor root (same contract as the old body check).
const Node& require_supervisor_root(const std::unique_ptr<Node>& root) {
    if (!root || !root->is_supervisor()) {
        throw std::runtime_error("root must be a supervisor");
    }
    return *root;
}
}  // namespace

// Point the file-local engine logger at the node logger the worker injected.
void Supervisor::set_logger(::theia::runtime::Logger* lg) noexcept {
    g_engine_logger = lg;
}

Supervisor::Supervisor(std::unique_ptr<Node> root,
                        std::string root_dir,
                        std::string machine_name)
    // require_supervisor_root() runs first (its arg `root` is evaluated before
    // the move into root_node_), validating + yielding the tree the Registry is
    // built from. Member-init order is declaration order: root_node_, then
    // registry_ — both see the same valid tree.
    : root_node_((require_supervisor_root(root), std::move(root))),
      registry_(*root_node_),
      root_dir_(std::move(root_dir)) {
    root_ = &root_node_->sup;

    start_time_      = std::chrono::steady_clock::now();
    last_heartbeat_  = start_time_;
    last_snapshot_   = start_time_;

    // machine_name is retained for logs only (etcd dropped). Resolve a
    // sensible default but don't act on it beyond logging.
    if (machine_name.empty()) {
        const char* h = std::getenv("HOSTNAME");
        if (h && *h) {
            machine_name = h;
        } else {
            char buf[256] = {};
            machine_name = (gethostname(buf, sizeof(buf) - 1) == 0)
                               ? std::string(buf) : std::string("unknown");
        }
    }
    (void)machine_name;

    // Block SIGCHLD only and read it via signalfd — child reaping is the
    // engine's concern. SIGTERM/SIGINT are NOT ours: the generic gen-app main.cc
    // owns process termination (std::signal → stop nodes → exit), the same as
    // every FC. (Previously this also blocked SIGTERM/SIGINT, stealing them from
    // main's handler → the process hung on SIGTERM.) pthread_sigmask, NOT
    // sigprocmask: this runs on the worker thread (GenRunnable::do_start) of a
    // multithreaded process, where sigprocmask is undefined behavior.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    if (int e = pthread_sigmask(SIG_BLOCK, &mask, nullptr); e != 0) {
        throw std::runtime_error(std::string("pthread_sigmask: ") + std::strerror(e));
    }
    signal_fd_ = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (signal_fd_ < 0) {
        throw std::runtime_error(std::string("signalfd: ") + std::strerror(errno));
    }

    // Command-queue wake. The control node (TipcMux thread) enqueue()/call()s a
    // typed ExecCommand + writes this fd; the select() loop adds it to its
    // fd_set and drains/dispatches the queued commands on the loop thread.
    // EFD_NONBLOCK so the loop's drain never blocks; the counter semantics
    // collapse N pending writes into one readable event (we drain the whole
    // queue regardless of the counter value).
    cmd_eventfd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (cmd_eventfd_ < 0) {
        throw std::runtime_error(std::string("eventfd: ") + std::strerror(errno));
    }
}

Supervisor::~Supervisor() {
    if (signal_fd_ >= 0) close(signal_fd_);
    if (cmd_eventfd_ >= 0) close(cmd_eventfd_);
}

// Safe from any thread / the FC shell's do_stop(). Set the flag AND wake the
// select() loop (via the command eventfd) so shutdown is observed promptly.
void Supervisor::request_shutdown() {
    shutdown_requested_.store(true);
    if (cmd_eventfd_ >= 0) {
        uint64_t one = 1;
        ssize_t n = ::write(cmd_eventfd_, &one, sizeof(one));
        (void)n;
    }
}

// ---------------------------------------------------------------------------
// Control-surface impls — LOOP-THREAD ONLY. Invoked exclusively by dispatch()
// (drain_commands) on the select-loop thread, which is the single writer of the
// tree + config tables. No lock: the actor queue (cmd_mutex_) already serialized
// ingress; once a command runs here it owns the state uncontended.
// ---------------------------------------------------------------------------

uint32_t Supervisor::ctl_start_child(const std::string& parent_sup,
                                     const std::string& name,
                                     const std::vector<std::string>& start_cmd,
                                     int restart, int shutdown, int type,
                                     const std::vector<std::string>& modules) {
    uint32_t status = 0;
    do_start_child(parent_sup, name, start_cmd, restart, shutdown, type,
                   modules, status);
    return status;
}

uint32_t Supervisor::ctl_delete_child(const std::string& name) {
    return do_delete_child(name);
}
uint32_t Supervisor::ctl_restart_child(const std::string& name) {
    return do_restart_child(name);
}
uint32_t Supervisor::ctl_terminate_child(const std::string& name) {
    return do_terminate_child(name);
}
uint32_t Supervisor::ctl_suspend_child(const std::string& name) {
    return do_suspend_child(name);
}
uint32_t Supervisor::ctl_resume_child(const std::string& name) {
    return do_resume_child(name);
}

bool Supervisor::ctl_configure_trace(const std::string& target_node,
                                     bool enabled, uint32_t kind) {
    // Validate the target resolves to a real reporting node BEFORE storing, so a
    // typo'd / non-reporting name fails the control reply instead of silently
    // "succeeding". The Registry is the immutable manifest index — same answer
    // on any thread. STORE ONLY — SupervisorCtl does the cast.
    if (!registry_.resolve(target_node).ok) {
        return false;
    }
    apply_trace_config(target_node, kind, enabled);
    return true;
}

bool Supervisor::ctl_configure_log_level(const std::string& target_node,
                                         uint32_t level) {
    if (!registry_.resolve(target_node).ok) {
        return false;
    }
    apply_log_level(target_node, level);
    return true;
}

std::vector<TraceConfigRow> Supervisor::ctl_get_trace_config() {
    std::vector<TraceConfigRow> out;
    for (const auto& outer : trace_configs_) {
        for (const auto& kv : outer.second) {
            if (!kv.second) continue;   // only enabled kinds
            TraceConfigRow r;
            r.target_node = outer.first;
            r.kind        = kv.first;   // inner key = TraceKind ordinal (#403)
            out.push_back(std::move(r));
        }
    }
    return out;
}

std::vector<LogLevelRow> Supervisor::ctl_get_log_level() {
    // Every reporting node, effective level = boot ⊕ override. Boot is the
    // worker's THEIA_LOG_LEVEL spawn env (the name the FC's main.cc parsed);
    // override is log_levels_[name] (set by a ConfigureLogLevel push, which
    // keys by the node OR worker name). One row per reporting node.
    std::vector<LogLevelRow> out;
    for (WorkerNode* w : all_workers(*root_)) {
        // Boot ordinal: parse the worker's env (default Info when unset).
        auto it = w->env.find("THEIA_LOG_LEVEL");
        const uint32_t boot = static_cast<uint32_t>(
            ::theia::runtime::parse_log_level(
                it != w->env.end() ? it->second : "info"));
        for (const NodeInfo& ni : w->nodes) {
            if (!ni.reporting) continue;
            LogLevelRow r;
            r.target_node = ni.name;
            r.boot_level  = boot;
            // Override keyed by the node name OR the worker name.
            uint32_t eff = boot;
            bool overridden = false;
            for (const std::string& key : {ni.name, w->name}) {
                auto o = log_levels_.find(key);
                if (o != log_levels_.end() && o->second != kNoLevel) {
                    eff = o->second; overridden = true; break;
                }
            }
            r.level       = eff;
            r.is_override = overridden;
            out.push_back(std::move(r));
        }
    }
    return out;
}

void Supervisor::ctl_get_tombstone(const std::string& child_name,
                                   ExecReply& rep) {
    rep.tomb_found = false;
    WorkerNode* w = find_worker_by_name(child_name);
    if (!w) return;
    SupervisorNode* sup = supervisor_of(*w);
    const std::string dir = find_tombstone_dir(sup);
    if (dir.empty()) return;
    // The child may have restarted since it cored, so its current pid won't
    // match the tombstone's. Find the newest tombstone for this NAME (any pid).
    const std::string path = locate_tombstone_prefixed(dir, child_name + "-");
    if (path.empty()) return;

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return;
    const auto total = static_cast<uint64_t>(in.tellg());
    in.seekg(0);
    // Cap UNDER the ~48KB TIPC gen_server reply limit; the .art reply's
    // `content` is a 40KB nanopb char[]. A longer tombstone is truncated; the
    // path + truncated flag let the operator read the rest at the source.
    constexpr size_t kCap = 40 * 1024;
    const size_t take = std::min<size_t>(total, kCap);
    std::string buf(take, '\0');
    in.read(&buf[0], static_cast<std::streamsize>(take));

    rep.tomb_found     = true;
    rep.tomb_path      = path;
    rep.tomb_total     = total;
    rep.tomb_content   = std::move(buf);
    rep.tomb_truncated = (total > take);
}

std::vector<TreeRow> Supervisor::ctl_get_tree() {
    // Same topological walk as emit_tree_stream, collected into a flat list.
    // Synthesize the <worker>_sup bracket row for reporting workers (#364) so
    // the rebuilt tree matches the firehose hierarchy.
    std::vector<TreeRow> out;
    std::function<void(const SupervisorNode&, const std::string&)> walk =
        [&](const SupervisorNode& sup, const std::string& parent) {
            TreeRow s;
            s.name          = sup.name;
            s.parent_name   = parent;
            s.kind          = 1;   // supervisor
            s.pid           = -1;
            s.state         = 2;   // running
            s.restart_count = sup.restart_count;  // cumulative, not the window
            s.strategy      = to_string(sup.strategy);
            out.push_back(std::move(s));

            for (const auto& c : sup.children) {
                if (c->is_worker()) {
                    std::string worker_parent = sup.name;
                    bool has_reporting = false;
                    for (const auto& ni : c->worker.nodes) {
                        if (ni.reporting) { has_reporting = true; break; }
                    }
                    if (has_reporting) {
                        TreeRow synth;
                        synth.name        = c->worker.name + "_sup";
                        synth.parent_name = sup.name;
                        synth.kind        = 1;
                        synth.pid         = -1;
                        synth.state       = 2;
                        synth.strategy    = "one_for_all";
                        // The synthetic bracket wraps ONE worker — its
                        // "restarts" is that worker's cumulative count, so the
                        // wrapper row doesn't show a misleading 0.
                        synth.restart_count = c->worker.restart_count;
                        out.push_back(std::move(synth));
                        worker_parent = c->worker.name + "_sup";
                    }
                    const WorkerNode& w = c->worker;
                    TreeRow r;
                    r.name           = w.name;
                    r.parent_name    = worker_parent;
                    r.kind           = 0;  // worker
                    r.pid            = w.pid;
                    r.state          = (w.pid > 0) ? 2u : 0u;
                    if (w.terminating) r.state = 3u;
                    r.restart_count  = w.restart_count;
                    r.last_exit_code = w.last_exit_code;
                    r.flags          = w.flags;
                    for (size_t i = 0; i < w.start_cmd.size(); ++i) {
                        if (i) r.start_cmd += ' ';
                        r.start_cmd += w.start_cmd[i];
                    }
                    // Resource metrics — same source cast_node_state() uses, so
                    // GetTree (tdb ps / GUI poll) carries the live cpu/mem/uptime
                    // instead of zeros. uptime = now - last_start; cpu/mem/threads
                    // from the per-tick /proc sample (sample_[pid]).
                    if (w.pid > 0) {
                        r.uptime_ms = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now()
                                - w.last_start).count());
                        auto sit = sample_.find(w.pid);
                        if (sit != sample_.end()) {
                            const auto& ps = sit->second;
                            r.cpu_pct   = ps.cpu_pct;
                            r.rss_kb    = ps.rss_kb;
                            r.vsz_kb    = ps.vsz_kb;
                            r.shared_kb = ps.shared_kb;
                            r.data_kb   = ps.data_kb;
                            r.threads   = ps.threads;
                            for (const auto& te : ps.threads_detail) {
                                TreeThreadRow tt;
                                tt.tid               = te.second.tid;
                                tt.comm              = te.second.comm;
                                tt.cpu_pct           = te.second.cpu_pct;
                                tt.sched_policy      = te.second.sched_policy;
                                tt.sched_priority    = te.second.sched_priority;
                                tt.nice              = te.second.nice;
                                tt.cpu_affinity_mask = te.second.cpu_affinity_mask;
                                tt.last_cpu          = te.second.last_cpu;
                                r.threads_detail.push_back(std::move(tt));
                            }
                            r.sockets = ps.sockets;   // full detail (off wire)
                            // Per-node TIPC traffic summary (rides the wire).
                            for (const auto& sk : ps.sockets) {
                                r.tipc_rx += sk.rx_queue;
                                r.tipc_tx += sk.tx_queue;
                            }
                        }
                    }
                    out.push_back(std::move(r));

                    // One leaf row per artheia node hosted in this worker
                    // (SmDaemon/SmGate, CounterNode/DriverNode/TickerNode, ...).
                    // kind=2 = node; share the worker's pid (all nodes run as
                    // threads in the one process). Carry the node's TIPC addr in
                    // start_cmd (the generic detail field the renderer shows as a
                    // trailing tag) so `tdb ps` surfaces where each node binds —
                    // the same address the supervisor pushes trace config to.
                    for (const auto& ni : w.nodes) {
                        TreeRow n;
                        n.name        = ni.name;
                        n.parent_name = w.name;
                        n.kind        = 2;  // node
                        n.pid         = w.pid;
                        n.state       = (w.pid > 0) ? 2u : 0u;
                        if (w.terminating) n.state = 3u;
                        n.flags       = ni.reporting ? 0u : 0x100u;  // non-reporting bit
                        if (!ni.tipc_type.empty()) {
                            n.start_cmd = "tipc=" + ni.tipc_type;
                            if (!ni.tipc_instance.empty() &&
                                ni.tipc_instance != "0") {
                                n.start_cmd += ":" + ni.tipc_instance;
                            }
                        }
                        out.push_back(std::move(n));
                    }
                } else {
                    walk(c->sup, sup.name);
                }
            }
        };
    walk(*root_, "");
    return out;
}

SystemInfoData Supervisor::ctl_get_system_info() {
    SystemInfoData info;
    do_get_system_info(info);
    return info;
}

// HeartbeatReport ingress — ported from the old on_inbound_frame kTagHeartbeat
// path. Updates the watchdog table + fires the saved trace/log re-push on the
// first beat after a gap (#361).
void Supervisor::ctl_on_heartbeat(const std::string& node_name, pid_t pid,
                                  uint64_t seq) {
    auto now = std::chrono::steady_clock::now();
    auto& s = heartbeats_[pid];
    s.last_seen = now;
    s.last_seq  = seq;
    engine_log().debug("heartbeat node=" + node_name + " pid=" +
                       std::to_string(static_cast<int>(pid)) + " seq=" +
                       std::to_string(seq));

    auto it = last_heartbeat_by_child_.find(node_name);
    bool fire_push = (it == last_heartbeat_by_child_.end()) ||
                     (now - it->second > std::chrono::seconds(5));
    last_heartbeat_by_child_[node_name] = now;
    if (fire_push) {
        push_trace_config_to_child(node_name);
        push_log_level_to_child(node_name);
    }
}

// SendTimeoutReport ingress — ported from the old on_inbound_frame
// kTagSendTimeout path. Surfaced as a kind=7 supervision event.
void Supervisor::ctl_on_send_timeout(const std::string& caller,
                                     const std::string& callee,
                                     const std::string& iface,
                                     const std::string& method,
                                     uint32_t budget_ms, uint32_t observed_ms) {
    std::ostringstream detail;
    detail << caller << " → " << callee << " " << iface << "." << method
           << " budget=" << budget_ms << "ms observed=" << observed_ms << "ms";
    emit_event(/*kind=*/7, /*worker=*/nullptr, /*sup=*/nullptr,
               /*exit_code=*/0, /*tombstone=*/"", detail.str());
}

// ---------------------------------------------------------------------------
// Tree traversal helpers
// ---------------------------------------------------------------------------

std::vector<WorkerNode*> Supervisor::all_workers(SupervisorNode& sup) {
    std::vector<WorkerNode*> out;
    for (const auto& c : sup.children) {
        if (c->is_worker()) {
            out.push_back(&c->worker);
        } else {
            auto sub = all_workers(c->sup);
            out.insert(out.end(), sub.begin(), sub.end());
        }
    }
    return out;
}

SupervisorNode* Supervisor::supervisor_of(WorkerNode& w) {
    // BFS from the root.
    std::vector<SupervisorNode*> stack{root_};
    while (!stack.empty()) {
        SupervisorNode* sup = stack.back();
        stack.pop_back();
        for (const auto& c : sup->children) {
            if (c->is_worker()) {
                if (&c->worker == &w) return sup;
            } else {
                stack.push_back(&c->sup);
            }
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// fork/exec primitives
// ---------------------------------------------------------------------------

void Supervisor::start_worker(WorkerNode& w) {
    // Resolve relative argv[0] against root_dir.
    std::vector<std::string> argv = w.start_cmd;
    if (argv.empty()) {
        log_err("worker " + w.name + " has empty start_cmd");
        return;
    }
    if (!path_is_absolute(argv[0])) {
        argv[0] = root_dir_ + "/" + argv[0];
    }

    std::ostringstream msg;
    msg << "starting child " << w.name << ": " << join(argv);
    log_info(msg.str());

    // Resolve the tombstone dir BEFORE fork (no allocation in the child). The
    // child's main.cc installs the libtombstone fatal-signal handler pointed at
    // THEIA_TOMBSTONE_DIR, so a crash writes tombstone-<name>-<pid>-* there —
    // which GetTombstone later serves. Empty when no ancestor sets one.
    const std::string tombstone_dir = find_tombstone_dir(supervisor_of(w));

    // Captured BEFORE fork so the child can detect a parent that died in the
    // fork→prctl window (see PR_SET_PDEATHSIG below). This is the supervisor's
    // own pid at fork time.
    const pid_t parent_pid = ::getpid();

    pid_t pid = fork();
    if (pid < 0) {
        log_err("fork failed: " + std::string(std::strerror(errno)));
        return;
    }
    if (pid == 0) {
        // Child: unblock all signals, lead our own process group.
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, nullptr);
        setsid();

        // Parent-death signal: if the supervisor dies — including a hard
        // SIGKILL where shutdown_subtree() never runs — the kernel SIGKILLs
        // this child immediately, so it can't orphan to PPID 1 (still bound to
        // its TIPC names). setsid() above gives us our own process group but
        // does NOT tie our lifetime to the parent; PDEATHSIG does.
        //
        // Two caveats handled here:
        //  - PDEATHSIG is cleared across execvp's credential change ONLY if the
        //    exec changes the dumpable/uid state; a plain execvp of a same-user
        //    binary keeps it, so setting it pre-exec is sufficient.
        //  - Race: if the supervisor already died between fork() and this line,
        //    the signal we just armed would never fire. Re-check getppid() — if
        //    we've already been reparented (ppid != the supervisor's pid, i.e.
        //    1 or a subreaper), exit now rather than run orphaned.
        prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
        if (::getppid() != parent_pid) _exit(128);

        // Working dir.
        const std::string cwd = w.working_dir.empty() ? root_dir_ : w.working_dir;
        if (chdir(cwd.c_str()) < 0) {
            std::fprintf(stderr, "chdir(%s): %s\n", cwd.c_str(), std::strerror(errno));
            _exit(127);
        }

        // env: extend the inherited environment.
        for (const auto& kv : w.env) {
            setenv(kv.first.c_str(), kv.second.c_str(), 1);
        }
        // Tombstone output dir for the child's libtombstone handler.
        if (!tombstone_dir.empty())
            setenv("THEIA_TOMBSTONE_DIR", tombstone_dir.c_str(), 1);

        // THEIA_NODE_CFG — per-node CPU affinity + scheduler for the hosting
        // process's main.cc to apply to each node thread (apply_node_affinity).
        // Built from this worker's NodeInfo (rig NodeToCPUMapping). Encoding:
        //   <node>=cpu:<c>,<c>;sched:<policy>[:<prio>]  | <node2>=...
        // Only nodes that actually carry cfg are emitted; absent THEIA_NODE_CFG
        // means "nothing to pin" (the common case).
        {
            std::string cfg;
            for (const auto& ni : w.nodes) {
                if (ni.cpus.empty() && ni.sched.empty()) continue;
                if (!cfg.empty()) cfg += '|';
                cfg += ni.name;
                cfg += '=';
                bool first_field = true;
                if (!ni.cpus.empty()) {
                    cfg += "cpu:";
                    for (size_t i = 0; i < ni.cpus.size(); ++i) {
                        if (i) cfg += ',';
                        cfg += std::to_string(ni.cpus[i]);
                    }
                    first_field = false;
                }
                if (!ni.sched.empty()) {
                    if (!first_field) cfg += ';';
                    cfg += "sched:";
                    cfg += ni.sched;
                    if (ni.sched == "fifo" || ni.sched == "rr") {
                        cfg += ':';
                        cfg += std::to_string(ni.sched_prio);
                    }
                }
            }
            if (!cfg.empty()) setenv("THEIA_NODE_CFG", cfg.c_str(), 1);
        }

        // CPU affinity. AUTOSAR ProcessToMachineMapping flavour:
        //   shall_run_on:     positive list — only these cores.
        //   shall_not_run_on: negative list — every online core MINUS these.
        // Mutual exclusion enforced at load time. Empty == no constraint.
        if (!w.shall_run_on.empty() || !w.shall_not_run_on.empty()) {
            cpu_set_t mask;
            CPU_ZERO(&mask);
            if (!w.shall_run_on.empty()) {
                for (int c : w.shall_run_on) {
                    if (c >= 0 && c < CPU_SETSIZE) CPU_SET(c, &mask);
                }
            } else {
                // Online CPU count via sysconf — coarse but adequate.
                long n = sysconf(_SC_NPROCESSORS_ONLN);
                if (n <= 0) n = CPU_SETSIZE;
                for (long c = 0; c < n && c < CPU_SETSIZE; ++c) CPU_SET(c, &mask);
                for (int c : w.shall_not_run_on) {
                    if (c >= 0 && c < CPU_SETSIZE) CPU_CLR(c, &mask);
                }
            }
            if (sched_setaffinity(0, sizeof(mask), &mask) < 0) {
                std::fprintf(stderr, "sched_setaffinity: %s\n", std::strerror(errno));
                // Soft-fail: continue with whatever affinity we have.
            }
        }

        // Per-process memory cap (RLIMIT_AS). RLIMIT is per-PROCESS, so this
        // bounds the whole child (all its node threads' address space). Applied
        // in the child, pre-exec — execvp keeps the rlimit. 0 = no cap. Soft-
        // fail: a too-low cap would just fail the child's own allocations, not
        // the supervisor.
        if (w.mem_limit_bytes > 0) {
            struct rlimit rl;
            rl.rlim_cur = static_cast<rlim_t>(w.mem_limit_bytes);
            rl.rlim_max = static_cast<rlim_t>(w.mem_limit_bytes);
            if (setrlimit(RLIMIT_AS, &rl) < 0) {
                std::fprintf(stderr, "setrlimit(RLIMIT_AS, %llu): %s\n",
                             static_cast<unsigned long long>(w.mem_limit_bytes),
                             std::strerror(errno));
            }
        }

        // Propagate CAP_SYS_NICE to the child so its main.cc can set realtime
        // scheduling (SCHED_FIFO/RR) on the node threads. A capability is NOT
        // inherited across execvp by default. Two steps:
        //   (1) add it to this (forked child) process's INHERITABLE set —
        //       PR_CAP_AMBIENT_RAISE requires the cap in BOTH Permitted AND
        //       Inheritable, but `setcap +eip` populates the file's inheritable,
        //       not the running process's, so we set it explicitly via capset;
        //   (2) raise it into the AMBIENT set so the exec'd child gets it
        //       Effective.
        // All a silent no-op when the supervisor lacks CAP_SYS_NICE in Permitted
        // (no `setcap cap_sys_nice+eip` on the supervisor binary) — rt-sched then
        // soft-fails in the child (affinity still works). The gateway's
        // cap_net_raw is granted on the GATEWAY binary directly, not here.
        {
            // capget the current header+data, OR in CAP_SYS_NICE inheritable.
            __user_cap_header_struct hdr{};
            hdr.version = _LINUX_CAPABILITY_VERSION_3;
            hdr.pid = 0;  // self
            __user_cap_data_struct data[2]{};
            if (syscall(SYS_capget, &hdr, data) == 0) {
                const int idx = CAP_TO_INDEX(CAP_SYS_NICE);
                const uint32_t bit = CAP_TO_MASK(CAP_SYS_NICE);
                data[idx].inheritable |= bit;   // need Inh for the ambient raise
                syscall(SYS_capset, &hdr, data);  // best-effort
            }
#if defined(PR_CAP_AMBIENT) && defined(PR_CAP_AMBIENT_RAISE)
            prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_SYS_NICE, 0, 0);
#endif
        }

        // exec argv.
        std::vector<char*> c_argv;
        for (auto& a : argv) c_argv.push_back(const_cast<char*>(a.c_str()));
        c_argv.push_back(nullptr);

        execvp(c_argv[0], c_argv.data());
        std::fprintf(stderr, "execvp(%s): %s\n", c_argv[0], std::strerror(errno));
        _exit(127);
    }

    // Parent.
    w.pid = pid;
    w.last_start = std::chrono::steady_clock::now();
    // #429 — the freshly-(re)started instance hasn't cored; clear CORE_DUMPED.
    // DEGRADED stays sticky (restart thrashing is about the budget window, not
    // this single instance) until the window ages out.
    w.flags &= ~1u;  // clear NodeFlag CORE_DUMPED (bit0)
    emit_event(/*kind=child_started*/0, &w, supervisor_of(w),
               /*exit_code=*/0, std::string{}, std::string{});
    emit_snapshot();

    // Config survives the (re)start: if this child has stored trace
    // config or a stored log level, arm a deferred re-push. The child
    // needs a grace window to bind its config-service TIPC socket before
    // a cast can land (same 500ms the SM startup handshake uses). This is
    // the crash-investigation re-apply for FCs that don't beat — the
    // heartbeat-after-gap path (#361) only covers reporting nodes.
    if (trace_configs_.count(w.name) || log_levels_.count(w.name)) {
        config_repush_due_[w.name] =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    }
}

void Supervisor::start_subtree(SupervisorNode& sup) {
    for (const auto& c : sup.children) {
        if (c->is_worker())     start_worker(c->worker);
        else                    start_subtree(c->sup);
    }
}

void Supervisor::stop_worker(WorkerNode& w) {
    if (w.pid <= 0) return;
    log_info("stopping " + w.name + " (pid=" + std::to_string(w.pid) + ")");

    // Mark as terminating before issuing the signal so reap() in the main
    // loop doesn't try to apply restart strategy when our own SIGCHLD
    // arrives — this stop is owned by the synchronous shutdown path.
    w.terminating = true;
    pid_t pgid = w.pid;  // setsid() in child means pid == pgid

    if (w.shutdown.kind == Shutdown::kBrutalKill) {
        ::killpg(pgid, SIGKILL);
    } else {
        ::killpg(pgid, SIGTERM);
    }

    // Wait for the child to actually exit.
    int status = 0;
    if (w.shutdown.kind == Shutdown::kInfinity) {
        while (waitpid(w.pid, &status, 0) < 0 && errno == EINTR) {}
    } else {
        // Poll with timeout.
        const int total_ms = (w.shutdown.kind == Shutdown::kBrutalKill)
                                 ? 2000
                                 : w.shutdown.timeout_ms;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(total_ms);
        bool reaped = false;
        while (std::chrono::steady_clock::now() < deadline) {
            pid_t r = waitpid(w.pid, &status, WNOHANG);
            if (r == w.pid) { reaped = true; break; }
            if (r < 0 && errno != ECHILD && errno != EINTR) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!reaped) {
            log_warn("SIGTERM timed out for " + w.name + ", SIGKILLing");
            ::killpg(pgid, SIGKILL);
            // Final reap (no timeout — should be near-instant now).
            while (waitpid(w.pid, &status, 0) < 0 && errno == EINTR) {}
        }
    }
    w.pid = -1;
    w.terminating = false;
}

void Supervisor::shutdown_subtree(SupervisorNode& sup) {
    // Reverse declared order so dependents go down before what they depend on.
    for (auto it = sup.children.rbegin(); it != sup.children.rend(); ++it) {
        if ((*it)->is_worker())     stop_worker((*it)->worker);
        else                        shutdown_subtree((*it)->sup);
    }
}

// ---------------------------------------------------------------------------
// Restart policy
// ---------------------------------------------------------------------------

void Supervisor::on_child_exit(WorkerNode& w, int return_code, pid_t old_pid) {
    SupervisorNode* sup = supervisor_of(w);
    if (!sup) {
        log_warn("exit from unknown child " + w.name);
        return;
    }

    const bool abnormal = (return_code != 0);
    {
        std::ostringstream msg;
        msg << "child " << w.name << " exited (code=" << return_code << ", "
            << (abnormal ? "abnormal" : "normal") << ") — sup=" << sup->name
            << " strategy=" << to_string(sup->strategy);
        log_info(msg.str());
    }
    if (return_code == 127) {
        // Conventional execvp-failed code from our fork() path; the child
        // wrote a diagnostic to stderr before _exit(127). Surface it so
        // a missing binary doesn't look like a generic crash.
        log_err("child " + w.name + " failed to exec (code 127) — "
                "check start_cmd and PATH");
    }
    // Tombstone surfacing: when the child died from a fatal signal
    // (rc < 0 in our convention), look for a tombstone file written by
    // libtombstone in the configured directory and log its path.
    if (return_code < 0) {
        std::string dir = find_tombstone_dir(sup);
        if (!dir.empty()) {
            std::string ts = locate_tombstone(dir, w.name, old_pid);
            if (!ts.empty()) {
                log_err("tombstone for " + w.name + " (pid=" +
                        std::to_string(old_pid) + "): " + ts);
                // Tree-wide cumulative for the HealthBeacon (GUI "tombstones").
                total_tombstones_++;
            }
        }
    }

    if (w.restart == RestartType::Temporary)             return;
    if (w.restart == RestartType::Transient && !abnormal) return;

    // #429 — record this restart in the sliding intensity window and check the
    // budget FIRST. If exceeded, the node is torn down (escalation), NOT
    // restarted — so we must NOT count it as a restart (the lifetime counter
    // tracks successful respawns, not the fatal final exit).
    if (!record_and_check_restart(*sup)) {
        std::ostringstream msg;
        msg << "supervisor " << sup->name << " exceeded restart intensity ("
            << sup->max_restarts << " in " << sup->max_seconds
            << "s) — escalating";
        log_err(msg.str());
        escalated_ = true;
        w.flags |= 2u;          // NodeFlag DEGRADED (bit1)
        cast_node_state(w);     // last gasp before escalation tears down
        shutdown_requested_.store(true);
        return;
    }
    // A respawn WILL happen: count it (worker + the supervisor's cumulative
    // stat stay in lockstep — see bump_restart_count_).
    bump_restart_count_(w, *sup);
    // Nearing exhaustion: history is at the budget ceiling.
    if (static_cast<int>(sup->restart_history.size()) >= sup->max_restarts) {
        w.flags |= 2u;          // DEGRADED
    }

    switch (sup->strategy) {
        case RestartStrategy::OneForOne:
            start_worker(w);
            break;
        case RestartStrategy::OneForAll:
            restart_all(*sup);
            break;
        case RestartStrategy::RestForOne:
            restart_rest(*sup, w);
            break;
        case RestartStrategy::SimpleOneForOne:
            log_warn("simple_one_for_one not implemented; treating as one_for_one");
            start_worker(w);
            break;
    }
}

bool Supervisor::record_and_check_restart(SupervisorNode& sup) {
    const auto now = std::chrono::steady_clock::now();
    const auto cutoff = now - std::chrono::seconds(sup.max_seconds);
    auto& hist = sup.restart_history;
    hist.erase(
        std::remove_if(hist.begin(), hist.end(),
                       [&](const auto& t) { return t < cutoff; }),
        hist.end());
    hist.push_back(now);
    return static_cast<int>(hist.size()) <= sup.max_restarts;
}

void Supervisor::bump_restart_count_(WorkerNode& w, SupervisorNode& sup) {
    // Both cumulative, monotonic — kept in lockstep so the snapshot's
    // "restarts" column reads consistently across row kinds (process =
    // w.restart_count, supervisor = sup.restart_count). The immediate
    // supervisor owns the restart (its strategy fired), matching how
    // restart_history is per-supervisor.
    w.restart_count++;
    sup.restart_count++;
    // Tree-wide cumulative for the HealthBeacon (GUI Load panel "restarts").
    total_restarts_++;
}

void Supervisor::restart_all(SupervisorNode& sup) {
    log_info("one_for_all: restarting all of sup=" + sup.name);
    auto workers = all_workers(sup);
    // Stop reverse, start forward.
    for (auto it = workers.rbegin(); it != workers.rend(); ++it) stop_worker(**it);
    for (auto* w : workers) start_worker(*w);
}

void Supervisor::restart_rest(SupervisorNode& sup, WorkerNode& failed) {
    log_info("rest_for_one: restarting " + failed.name + " and downstream in sup=" + sup.name);

    // Walk sup.children, find the entry containing `failed`, gather it +
    // every later entry's workers.
    std::vector<WorkerNode*> affected;
    bool seen = false;
    for (const auto& c : sup.children) {
        if (!seen) {
            if (c->is_worker()) {
                if (&c->worker == &failed) {
                    seen = true;
                    affected.push_back(&c->worker);
                }
            } else {
                // Does this subtree contain `failed`?
                auto in_sub = all_workers(c->sup);
                bool contains = false;
                for (auto* w : in_sub) {
                    if (w == &failed) { contains = true; break; }
                }
                if (contains) {
                    seen = true;
                    affected.insert(affected.end(), in_sub.begin(), in_sub.end());
                }
            }
            continue;
        }
        // After `seen`: include the entire entry.
        if (c->is_worker()) {
            affected.push_back(&c->worker);
        } else {
            auto sub = all_workers(c->sup);
            affected.insert(affected.end(), sub.begin(), sub.end());
        }
    }

    for (auto it = affected.rbegin(); it != affected.rend(); ++it) stop_worker(**it);
    for (auto* w : affected) start_worker(*w);
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Executor command surface — the actor ingress. enqueue()/call() push a typed
// ExecCommand and wake the loop; drain_commands() dispatches each on the loop
// thread. cmd_mutex_ guards ONLY the queue.
// ---------------------------------------------------------------------------

namespace {
// Wake the select() loop: eventfd write of 1 increments the counter; coalesces
// with other pending writes (the loop drains the whole queue regardless).
void wake_loop(int fd) {
    if (fd < 0) return;
    uint64_t one = 1;
    ssize_t n = ::write(fd, &one, sizeof(one));
    (void)n;  // best-effort; a full counter still leaves the fd readable
}
}  // namespace

// Fire-and-forget cast. Returns immediately; the command runs LATER on the loop
// thread. Callable from any thread (SupervisorCtl's TipcMux thread).
void Supervisor::enqueue(ExecCommand cmd) {
    cmd.reply = nullptr;  // a cast never carries a reply channel
    {
        std::lock_guard<std::mutex> lk(cmd_mutex_);
        cmd_queue_.push_back(std::move(cmd));
    }
    wake_loop(cmd_eventfd_);
}

// Synchronous call. Attaches a single per-call promise, enqueues, and blocks on
// the future until the loop thread dispatches the command and fills the reply.
// If the loop has already shut down, returns a default ExecReply (best-effort:
// the future would otherwise hang — guarded by the shutdown check).
ExecReply Supervisor::call(ExecCommand cmd) {
    // Re-entrant guard: a call() issued FROM the loop thread must NOT
    // enqueue+block — only the loop drains the queue, so it would self-deadlock.
    // Dispatch inline instead. (No current caller does this — resolve now reads
    // the lock-free Registry, not a call — but the guard is cheap insurance
    // against a future loop-thread call().)
    if (std::this_thread::get_id() == loop_tid_) {
        std::promise<ExecReply> prom;
        auto fut = prom.get_future();
        cmd.reply = &prom;
        dispatch(cmd);           // fills the promise synchronously, same thread
        return fut.get();
    }
    if (shutdown_requested_.load()) return ExecReply{};
    std::promise<ExecReply> prom;
    auto fut = prom.get_future();
    cmd.reply = &prom;
    {
        std::lock_guard<std::mutex> lk(cmd_mutex_);
        cmd_queue_.push_back(std::move(cmd));
    }
    wake_loop(cmd_eventfd_);
    return fut.get();
}

// Dispatch ONE command on the loop thread. The single switch that maps each
// typed op to its ctl_* impl; for CALL-shaped ops, fills cmd.reply. No lock —
// the loop is the single writer.
void Supervisor::dispatch(ExecCommand& cmd) {
    using Op = ExecCommand::Op;
    ExecReply rep;
    switch (cmd.op) {
        case Op::StartChild:
            rep.status = ctl_start_child(cmd.parent, cmd.name, cmd.start_cmd,
                                         cmd.restart, cmd.shutdown, cmd.type,
                                         cmd.modules);
            break;
        case Op::DeleteChild:
            rep.status = ctl_delete_child(cmd.name);
            break;
        case Op::RestartChild:
            rep.status = ctl_restart_child(cmd.name);
            break;
        case Op::SuspendChild:
            rep.status = ctl_suspend_child(cmd.name);
            break;
        case Op::ResumeChild:
            rep.status = ctl_resume_child(cmd.name);
            break;
        case Op::TerminateChild:
            rep.status = ctl_terminate_child(cmd.name);
            break;
        case Op::OnHeartbeat:
            ctl_on_heartbeat(cmd.name, cmd.pid, cmd.seq);
            break;
        case Op::OnSendTimeout:
            ctl_on_send_timeout(cmd.name, cmd.callee, cmd.iface, cmd.method,
                                cmd.budget_ms, cmd.observed_ms);
            break;
        case Op::ConfigureTrace:
            rep.ok = ctl_configure_trace(cmd.name, cmd.enabled, cmd.kind);
            rep.status = rep.ok ? 0u : 4u;
            break;
        case Op::ConfigureLogLevel:
            rep.ok = ctl_configure_log_level(cmd.name, cmd.level);
            rep.status = rep.ok ? 0u : 4u;
            break;
        case Op::GetTree:
            rep.tree = ctl_get_tree();
            break;
        case Op::GetSystemInfo:
            rep.sysinfo = ctl_get_system_info();
            break;
        case Op::GetTraceConfig:
            rep.trace_cfg = ctl_get_trace_config();
            break;
        case Op::GetLogLevelConfig:
            rep.log_cfg = ctl_get_log_level();
            break;
        case Op::GetTombstone:
            ctl_get_tombstone(cmd.name, rep);
            break;
        case Op::GetHealth:
            ctl_get_health(rep);
            break;
        case Op::Shutdown:
            request_shutdown();
            break;
    }
    if (cmd.reply) cmd.reply->set_value(std::move(rep));
}

// Drain the queue and dispatch every command on the LOOP THREAD. Called once
// per select() iteration before reap/sample/emit, so control dispatch is
// single-threaded with all other state mutation. We swap the queue out under
// the lock, then dispatch with the lock released (a command could enqueue).
void Supervisor::drain_commands() {
    // Drain the eventfd counter (non-blocking) so the loop doesn't spin.
    if (cmd_eventfd_ >= 0) {
        uint64_t cnt = 0;
        ssize_t n = ::read(cmd_eventfd_, &cnt, sizeof(cnt));
        (void)n;  // EAGAIN when not readable — fine
    }
    std::deque<ExecCommand> batch;
    {
        std::lock_guard<std::mutex> lk(cmd_mutex_);
        batch.swap(cmd_queue_);
    }
    for (auto& cmd : batch) {
        dispatch(cmd);
    }
}

int Supervisor::run() {
    loop_tid_ = std::this_thread::get_id();  // claim loop-thread identity
    log_info("supervisor starting (root=" + root_->name + ")");
    start_subtree(*root_);

    // The control surface is provided by the gen-app SupervisorCtl node
    // (bound on the FC's config_mux at 0x80020001); it enqueue()/call()s each
    // op into this engine. No ControlServer / TipcPublisher here any more —
    // this loop is purely the supervision state owner.

    while (!shutdown_requested_.load()) {
        // Wait for signalfd OR the command-queue eventfd readable; budget 1s
        // so we wake periodically for heartbeat / snapshot ticks anyway.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(signal_fd_, &rfds);
        if (cmd_eventfd_ >= 0) FD_SET(cmd_eventfd_, &rfds);
        int max_fd = signal_fd_;
        if (cmd_eventfd_ > max_fd) max_fd = cmd_eventfd_;
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int rv = ::select(max_fd + 1, &rfds, nullptr, nullptr, &tv);
        if (rv < 0) {
            if (errno == EINTR) continue;
            log_err("select: " + std::string(std::strerror(errno)));
            break;
        }

        if (rv > 0 && FD_ISSET(signal_fd_, &rfds)) {
            // Drain the signalfd.
            struct signalfd_siginfo si;
            while (true) {
                ssize_t n = ::read(signal_fd_, &si, sizeof(si));
                if (n < 0) {
                    if (errno == EAGAIN) break;
                    if (errno == EINTR)  continue;
                    log_err("read signalfd: " + std::string(std::strerror(errno)));
                    break;
                }
                if (n != sizeof(si)) break;

                // Only SIGCHLD is blocked into this signalfd now; reap() below
                // handles it. SIGTERM/SIGINT are owned by the generic main.cc
                // (which calls request_shutdown via worker.stop → do_stop), so
                // they never arrive here. Any other signo is ignored.
                (void)si;
            }
        }

        // Drain + dispatch any control commands enqueued by SupervisorCtl
        // (TipcMux thread) FIRST, on THIS loop thread, so the ctl_* impls
        // (→ do_*/apply_*) run single-threaded with reap/sample/emit below.
        // This IS the actor model: one writer (this loop), no lock on the tree,
        // no fork-under-lock, no reap delayed behind a control caller.
        drain_commands();

        // Reap any exited workers, regardless of whether select returned a
        // signalfd event — we may have missed coalesced SIGCHLDs.
        reap();

        // Deferred config re-apply on (re)start. A child armed in
        // start_worker() whose grace window has elapsed gets its stored
        // trace config + log level re-pushed — this is how the trace you
        // armed survives a CRASH (the heartbeat-after-gap path only covers
        // FCs that beat; sm does not). Crash-investigation re-apply.
        if (!config_repush_due_.empty()) {
            auto now2 = std::chrono::steady_clock::now();
            for (auto it = config_repush_due_.begin();
                 it != config_repush_due_.end(); ) {
                if (now2 >= it->second) {
                    push_trace_config_to_child(it->first);
                    push_log_level_to_child(it->first);
                    it = config_repush_due_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // (Inbound control / HeartbeatReport / SendTimeoutReport now arrive
        // on SupervisorCtl's TipcMux and reach the engine via enqueue()/call();
        // no socket to drain here.)

        // Periodic emissions. The .art file's heartbeat_period_ms /
        // snapshot_period_ms params are the canonical schedule; hardcoded
        // here until the param-from-manifest plumbing lands.
        using namespace std::chrono;
        const auto now = steady_clock::now();
        if (now - last_heartbeat_ >= milliseconds(1000)) {
            emit_health();
            // Same tick: scan for nodes that haven't reported a
            // HeartbeatReport in too long; SIGTERM them and let the
            // strategy restart. Nodes with no recorded heartbeat are
            // exempt (not all daemons participate in alive supervision).
            //
            // BUT skip the watchdog if the engine loop was STALLED since the
            // last tick — stop_worker() blocks the loop for up to a child's
            // SIGTERM grace (com/per take ~5s on gRPC/etcd teardown), during
            // which NO heartbeats are processed. Counting that gap against every
            // child false-positives the whole tree → mass-kill → escalation
            // cascade (a single `restart com` took the tree down). If the tick
            // ran late (we expect ~1s; >2.5s means the loop was busy), forgive
            // this round so children get a clean window to beat again.
            const auto tick_gap = now - last_heartbeat_;
            if (tick_gap <= milliseconds(2500)) {
                check_heartbeats();
            } else {
                // Push every recorded last_seen FORWARD by the stall (gap minus
                // the expected ~1s tick), so the blocked time isn't charged
                // against children retroactively — each gets a full kMaxAge
                // window from now to send its next beat. A genuinely dead child
                // still won't beat and is caught next round.
                const auto shift = tick_gap - milliseconds(1000);
                for (auto& kv : heartbeats_) kv.second.last_seen += shift;
                std::ostringstream m;
                m << "watchdog: engine loop stalled "
                  << duration_cast<milliseconds>(tick_gap).count()
                  << "ms (e.g. a slow stop_worker) — forgiving the gap so live "
                     "children aren't false-positive killed";
                log_warn(m.str());
                check_heartbeats();
            }
            last_heartbeat_ = now;
        }
        // 1 Hz snapshot — gives htop-like refresh rate on the GUI side.
        // sample_procs() inside emit_snapshot normalises cpu% by actual
        // elapsed wall time so the rate can change without distortion.
        if (now - last_snapshot_ >= milliseconds(1000)) {
            emit_snapshot();
            last_snapshot_ = now;
        }
    }

    // The control surface (SupervisorCtl) is owned + stopped by the FC shell
    // (main.cc), not here. Just bring the children down.
    shutdown_subtree(*root_);
    log_info("supervisor exiting");
    // Non-zero exit code if we got here because of escalation: the root
    // supervisor blew through its restart budget and there's nothing
    // left to do. The init system should see this as a failure.
    return escalated_ ? 1 : 0;
}

void Supervisor::reap() {
    // Snapshot pid → worker map.
    auto workers = all_workers(*root_);
    std::map<pid_t, WorkerNode*> by_pid;
    for (auto* w : workers) {
        if (w->pid > 0) by_pid[w->pid] = w;
    }

    while (true) {
        int status = 0;
        pid_t pid = ::waitpid(-1, &status, WNOHANG);
        if (pid == 0)  return;     // no more children ready
        if (pid < 0) {
            if (errno == ECHILD) return;  // nothing to wait on
            if (errno == EINTR)  continue;
            log_err("waitpid: " + std::string(std::strerror(errno)));
            return;
        }
        auto it = by_pid.find(pid);
        if (it == by_pid.end()) continue;  // not one of ours

        int rc;
        if (WIFEXITED(status))       rc = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) rc = -WTERMSIG(status);
        else                          rc = -1;

        WorkerNode& w = *it->second;
        const bool was_terminating = w.terminating;
        const pid_t old_pid = w.pid;
        w.pid = -1;  // mark dead before potential re-fork
        w.last_exit_code = rc;

        // #429 — core_dumped flag: a fatal signal that dumped core. Surfaced
        // on the next NodeState so the GUI/test can see "this node cored".
        if (WIFSIGNALED(status) && WCOREDUMP(status)) {
            w.flags |= 1u;  // NodeFlag CORE_DUMPED (bit0)
        }

        if (was_terminating || w.held) {
            // stop_worker() owns this exit (terminating), OR this worker is
            // HELD for test mocking (held) — either way DON'T apply the
            // restart strategy. A held worker stays down until ResumeChild;
            // a probe can bind its TIPC address meanwhile.
            log_info("child " + w.name + " stopped (" +
                     (w.held ? "held" : "terminating") + " path)");
            cast_node_state(w);
            continue;
        }
        on_child_exit(w, rc, old_pid);
    }
}


void Supervisor::emit_event(uint32_t kind,
                            const WorkerNode* worker,
                            const SupervisorNode* sup,
                            int exit_code,
                            const std::string& tombstone_path,
                            const std::string& detail) {
    if (!emit_.on_event) return;
    EventData ev;
    ev.kind            = kind;
    ev.timestamp_ms    = epoch_ms();
    if (worker) ev.child_name = worker->name;
    if (sup)    ev.supervisor_name = sup->name;
    ev.pid             = worker ? worker->pid : -1;
    ev.exit_code       = exit_code;
    if (sup) ev.strategy = to_string(sup->strategy);
    ev.tombstone_path  = tombstone_path;
    ev.detail          = detail;
    emit_.on_event(ev);
}

HealthData Supervisor::compute_health() {
    using namespace std::chrono;
    uint32_t total = 0, active = 0;
    for (auto* w : all_workers(*root_)) {
        ++total;
        if (w->pid > 0) ++active;
    }
    HealthData hb;
    hb.timestamp_ms     = epoch_ms();
    hb.uptime_ms        =
        duration_cast<milliseconds>(steady_clock::now() - start_time_).count();
    hb.generation       = generation_;
    hb.total_workers    = total;
    hb.active_workers   = active;
    hb.total_restarts   = total_restarts_;
    hb.total_tombstones = total_tombstones_;
    return hb;
}

void Supervisor::emit_health() {
    if (!emit_.on_health) return;
    emit_.on_health(compute_health());
}

// GetHealth control op — com polls this in its Subscribe loop (alongside
// GetTree) to feed the GUI Load panel. Returns the same beacon emit_health()
// would broadcast right now.
void Supervisor::ctl_get_health(ExecReply& rep) {
    rep.health = compute_health();
    rep.ok = true;
}

// /proc reader helpers — see header for the design intent.
namespace {

bool read_proc_stat(pid_t pid, uint64_t* utime, uint64_t* stime,
                    uint32_t* threads) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(pid));
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char buf[1024];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    if (n == 0) return false;
    buf[n] = '\0';
    // Skip past the parenthesised comm; fields 14/15 are utime/stime,
    // field 20 is num_threads (proc(5)).
    char* rp = std::strrchr(buf, ')');
    if (!rp) return false;
    rp += 2;
    int skip = 11;     // jump from field 3 to field 14
    while (skip > 0 && *rp) {
        if (*rp == ' ') --skip;
        ++rp;
    }
    long long ut = 0, st = 0;
    long      th = 0;
    int matched = std::sscanf(rp, "%lld %lld %*d %*d %*d %*d %ld",
                              &ut, &st, &th);
    if (matched < 3) return false;
    *utime   = static_cast<uint64_t>(ut);
    *stime   = static_cast<uint64_t>(st);
    *threads = static_cast<uint32_t>(th);
    return true;
}

bool read_proc_status(pid_t pid, uint64_t* rss_kb, uint64_t* vsz_kb,
                       uint64_t* data_kb) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/status", static_cast<int>(pid));
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char line[256];
    bool got_rss = false, got_vsz = false, got_data = false;
    while (std::fgets(line, sizeof(line), f)) {
        if (!got_rss && std::strncmp(line, "VmRSS:", 6) == 0) {
            unsigned long v = 0;
            if (std::sscanf(line + 6, "%lu", &v) == 1) {
                *rss_kb = v; got_rss = true;
            }
        } else if (!got_vsz && std::strncmp(line, "VmSize:", 7) == 0) {
            unsigned long v = 0;
            if (std::sscanf(line + 7, "%lu", &v) == 1) {
                *vsz_kb = v; got_vsz = true;
            }
        } else if (!got_data && std::strncmp(line, "VmData:", 7) == 0) {
            unsigned long v = 0;
            if (std::sscanf(line + 7, "%lu", &v) == 1) {
                *data_kb = v; got_data = true;
            }
        }
        if (got_rss && got_vsz && got_data) break;
    }
    std::fclose(f);
    return got_rss && got_vsz;        // data_kb is best-effort
}

// /proc/<pid>/smaps_rollup is the cheap whole-VMA summary added in
// Linux 4.14 — single page of pre-summed counters instead of
// O(VMAs) lines of smaps. Returns the sum of Shared_Clean +
// Shared_Dirty (the "any other process could be mapping these too"
// resident bytes). Returns false on kernels too old or perms denied.
bool read_proc_smaps_rollup_shared(pid_t pid, uint64_t* shared_kb) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/smaps_rollup",
                  static_cast<int>(pid));
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    uint64_t sc = 0, sd = 0;
    bool got_sc = false, got_sd = false;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        if (!got_sc && std::strncmp(line, "Shared_Clean:", 13) == 0) {
            unsigned long v = 0;
            if (std::sscanf(line + 13, "%lu", &v) == 1) {
                sc = v; got_sc = true;
            }
        } else if (!got_sd && std::strncmp(line, "Shared_Dirty:", 13) == 0) {
            unsigned long v = 0;
            if (std::sscanf(line + 13, "%lu", &v) == 1) {
                sd = v; got_sd = true;
            }
        }
        if (got_sc && got_sd) break;
    }
    std::fclose(f);
    if (got_sc && got_sd) {
        *shared_kb = sc + sd;
        return true;
    }
    return false;
}

}  // namespace

// ----- per-thread sampling helpers -----------------------------------------
//
// /proc/<pid>/task/<tid>/stat layout (proc(5)):
//   tid (comm-with-parens) state ppid pgrp session tty_nr tpgid flags
//   minflt cminflt majflt cmajflt utime stime cutime cstime priority
//   nice num_threads itrealvalue starttime vsize rss rsslim startcode
//   endcode startstack kstkesp kstkeip signal blocked sigignore
//   sigcatch wchan nswap cnswap exit_signal processor rt_priority
//   policy delayacct_blkio_ticks guest_time cguest_time start_data
//   ...
// We want utime(14) stime(15) priority(18) nice(19) num_threads(20)
// last_cpu(39) rt_priority(40) policy(41).

namespace {

bool read_thread_stat(pid_t pid, uint32_t tid,
                       uint64_t* utime, uint64_t* stime,
                       int32_t* nice, uint32_t* last_cpu,
                       uint32_t* rt_priority, uint32_t* policy) {
    char path[80];
    std::snprintf(path, sizeof(path), "/proc/%d/task/%u/stat",
                  static_cast<int>(pid), tid);
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char buf[1024];
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    if (n == 0) return false;
    buf[n] = '\0';
    char* rp = std::strrchr(buf, ')');
    if (!rp) return false;
    rp += 2;     // skip ") "
    // Walk from field 3 to field 14 (utime).
    int skip = 11;
    while (skip > 0 && *rp) {
        if (*rp == ' ') --skip;
        ++rp;
    }
    long long ut = 0, st = 0, nc = 0;
    long      lcpu = 0, rtprio = 0, pol = 0;
    // utime (14) stime (15) [cutime cstime priority] (16..18 ignored)
    // nice (19) [num_threads itrealvalue starttime vsize rss rsslim
    // startcode endcode startstack kstkesp kstkeip signal blocked
    // sigignore sigcatch wchan nswap cnswap exit_signal] (20..38 ignored)
    // processor (39) rt_priority (40) policy (41).
    // Field layout (proc(5)) after comm-with-parens consumed:
    //   14 utime    15 stime    16 cutime  17 cstime   18 priority
    //   19 nice     20 num_threads  21 itrealvalue  22 starttime
    //   23 vsize    24 rss    25 rsslim   26 startcode  27 endcode
    //   28 startstack  29 kstkesp  30 kstkeip  31 signal  32 blocked
    //   33 sigignore  34 sigcatch  35 wchan  36 nswap  37 cnswap
    //   38 exit_signal  39 processor  40 rt_priority  41 policy
    int matched = std::sscanf(rp,
        "%lld %lld %*d %*d %*d %lld "          /* 14 ut, 15 st, 16-18 skip, 19 nice */
        "%*d %*d %*d "                         /* 20 num_thr, 21 itreal, 22 starttime */
        "%*d %*d %*d %*d %*d %*d %*d "         /* 23-29 vsize..kstkesp */
        "%*d %*d %*d %*d %*d %*d %*d %*d %*d " /* 30-38 kstkeip..exit_signal */
        "%ld %ld %ld",                         /* 39 processor, 40 rt_prio, 41 policy */
        &ut, &st, &nc, &lcpu, &rtprio, &pol);
    if (matched < 6) return false;
    *utime       = static_cast<uint64_t>(ut);
    *stime       = static_cast<uint64_t>(st);
    *nice        = static_cast<int32_t>(nc);
    *last_cpu    = static_cast<uint32_t>(lcpu);
    *rt_priority = static_cast<uint32_t>(rtprio);
    *policy      = static_cast<uint32_t>(pol);
    return true;
}

bool read_thread_comm(pid_t pid, uint32_t tid, std::string* out) {
    char path[80];
    std::snprintf(path, sizeof(path), "/proc/%d/task/%u/comm",
                  static_cast<int>(pid), tid);
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char buf[64] = {0};
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    if (n == 0) return false;
    // strip trailing newline
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
    out->assign(buf, n);
    return true;
}

// /proc/<pid>/task/<tid>/status has Cpus_allowed: <hex>; pack the
// first 64 bits into a uint64_t mask.
bool read_thread_affinity_mask(pid_t pid, uint32_t tid, uint64_t* mask) {
    char path[80];
    std::snprintf(path, sizeof(path), "/proc/%d/task/%u/status",
                  static_cast<int>(pid), tid);
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char line[256];
    bool ok = false;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "Cpus_allowed:", 13) == 0) {
            // Hex, possibly comma-separated u32 words MSW-first. Strip
            // commas, parse as one big hex; keep low 64 bits.
            std::string hex;
            for (char* p = line + 13; *p; ++p) {
                if (std::isxdigit(static_cast<unsigned char>(*p))) hex.push_back(*p);
            }
            if (!hex.empty()) {
                uint64_t v = 0;
                for (char c : hex) {
                    v = (v << 4) | static_cast<uint64_t>(
                        c >= 'a' ? c - 'a' + 10 :
                        c >= 'A' ? c - 'A' + 10 : c - '0');
                }
                *mask = v;
                ok = true;
            }
            break;
        }
    }
    std::fclose(f);
    return ok;
}

// List the TIDs under /proc/<pid>/task/. Skip non-numeric entries.
std::vector<uint32_t> list_tids(pid_t pid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/task", static_cast<int>(pid));
    DIR* d = opendir(path);
    std::vector<uint32_t> out;
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        out.push_back(static_cast<uint32_t>(std::strtoul(e->d_name, nullptr, 10)));
    }
    closedir(d);
    return out;
}

}  // namespace

namespace {

// The TIPC socket table, keyed by fs inode: per-socket receive/transmit queue
// depth + local/peer TIPC port + state. We join this against each child's
// /proc/<pid>/fd inodes (below) to attribute sockets to a worker.
//
// Source: `ss --tipc -e` (iproute2). The kernel's TIPC sock_diag dump returns
// its data in TIPC-specific netlink attributes (NOT the inet_diag_msg layout),
// and ss already parses them correctly — including the fs `ino:` that matches
// /proc/<pid>/fd. Re-implementing that attribute parse in-process buys little;
// ss is a ~5ms popen once per 1s tick. Best-effort: empty map (no sockets in
// the snapshot, never an error) if ss is absent or TIPC isn't up.
//
// Line shape (header skipped):
//   ESTAB 0 12 1833111100:3635388691 1833111100:2928584382 uid:1000 ino:63932542 sk:5010
//   state rq sq  local                 peer                  ...      inode        ...
std::map<uint64_t, supervisor::TreeSocketRow> read_tipc_sockets() {
    std::map<uint64_t, supervisor::TreeSocketRow> out;
    FILE* p = ::popen("ss --tipc -e 2>/dev/null", "r");
    if (!p) return out;
    char line[512];
    bool first = true;
    while (std::fgets(line, sizeof(line), p)) {
        if (first) { first = false; continue; }   // header row
        char state[32] = {0}, local[64] = {0}, peer[64] = {0};
        unsigned long rq = 0, sq = 0;
        if (std::sscanf(line, "%31s %lu %lu %63s %63s",
                        state, &rq, &sq, local, peer) < 4) {
            continue;
        }
        // ino:<n> appears later on the line.
        uint64_t inode = 0;
        if (const char* ip = std::strstr(line, "ino:")) {
            inode = std::strtoull(ip + 4, nullptr, 10);
        }
        if (!inode) continue;
        supervisor::TreeSocketRow row;
        row.inode    = inode;
        row.rx_queue = static_cast<uint32_t>(rq);
        row.tx_queue = static_cast<uint32_t>(sq);
        // Map ss's textual state to a small enum (0 unknown / 1 ESTAB / 2 LISTEN).
        if (std::strncmp(state, "ESTAB", 5) == 0)       row.state = 1;
        else if (std::strncmp(state, "LISTEN", 6) == 0) row.state = 2;
        row.local  = local;
        if (peer[0] && std::strcmp(peer, "*") != 0) row.remote = peer;
        out[inode] = std::move(row);
    }
    ::pclose(p);
    return out;
}

// The socket inodes a pid currently holds open: readlink each /proc/<pid>/fd/*
// entry, parse the "socket:[<inode>]" form. The supervisor is these children's
// parent and runs as the same uid, so it satisfies the PTRACE_MODE_READ check
// that guards /proc/<pid>/fd.
std::vector<uint64_t> list_pid_socket_inodes(pid_t pid) {
    std::vector<uint64_t> inodes;
    char dir[64];
    std::snprintf(dir, sizeof(dir), "/proc/%d/fd", static_cast<int>(pid));
    DIR* d = ::opendir(dir);
    if (!d) return inodes;
    struct dirent* ent;
    while ((ent = ::readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        char path[320], target[128];
        std::snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        ssize_t len = ::readlink(path, target, sizeof(target) - 1);
        if (len <= 0) continue;
        target[len] = '\0';
        // "socket:[12345]"
        if (std::strncmp(target, "socket:[", 8) == 0) {
            uint64_t ino = std::strtoull(target + 8, nullptr, 10);
            if (ino) inodes.push_back(ino);
        }
    }
    ::closedir(d);
    return inodes;
}

}  // namespace

void Supervisor::sample_procs() {
    // Compute the wall-clock interval since the previous sample to
    // normalise cpu% (jiffies → percentage of one CPU).
    auto now = std::chrono::steady_clock::now();
    double interval_s = 0.0;
    if (last_proc_sample_.time_since_epoch().count() != 0) {
        interval_s = std::chrono::duration<double>(
                         now - last_proc_sample_).count();
    }
    last_proc_sample_ = now;

    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;

    // TIPC socket table once per tick (inode → queue depths), joined per-pid
    // below. Empty when TIPC sock_diag is unavailable — sockets then stay empty.
    const std::map<uint64_t, TreeSocketRow> tipc_by_inode = read_tipc_sockets();

    // Build the set of live pids; drop the rest from the map.
    auto workers = all_workers(*root_);
    std::map<pid_t, ProcSample> next;
    for (auto* w : workers) {
        if (w->pid <= 0) continue;
        ProcSample s = sample_[w->pid];   // carry forward prev_jiffies
        uint64_t ut = 0, st = 0;
        uint32_t th = 0;
        bool stat_ok = read_proc_stat(w->pid, &ut, &st, &th);
        if (stat_ok) {
            uint64_t cur = ut + st;
            uint64_t delta = (s.prev_jiffies <= cur) ? (cur - s.prev_jiffies) : 0;
            const uint64_t prev = s.prev_jiffies;
            const bool fresh = (prev == 0);   // new pid / first sample (no basis)
            s.prev_jiffies = cur;
            s.threads      = th;
            double raw_pct = 0.0;
            if (interval_s > 0.0) {
                // jiffies per second = clk_tck. CPU% = delta / (clk_tck * interval) * 100
                raw_pct = static_cast<double>(delta) /
                          (static_cast<double>(clk_tck) * interval_s) * 100.0;
                if (raw_pct < 0.0)   raw_pct = 0.0;
                if (raw_pct > 6553.0) raw_pct = 6553.0;  // fits in u32 ×100
                // EWMA smoothing: the raw % is jiffy-quantized (1 jiffy ≈ 1% per
                // 1s tick), so a near-idle process oscillates 0↔1%. Blend it so
                // the GUI shows a steady low value instead of a sawtooth. alpha
                // 0.4 ≈ a few-tick time constant — responsive but not jittery.
                constexpr double kAlpha = 0.4;
                if (!s.cpu_ewma_init) { s.cpu_ewma = raw_pct; s.cpu_ewma_init = true; }
                else s.cpu_ewma = kAlpha * raw_pct + (1.0 - kAlpha) * s.cpu_ewma;
                s.cpu_pct = static_cast<uint32_t>(s.cpu_ewma * 100.0 + 0.5);
            }
            // Why-is-cpu-zero tracing (env THEIA_TRACE_CPU=1). For each running
            // worker, log the raw inputs so a 0%% reading is explainable:
            //   delta=0          → genuinely idle this window (<1 jiffy used) —
            //                      the common cause of the val-0-0-0-val pattern;
            //                      sub-(1000/clk_tck)ms of CPU quantizes to 0.
            //   prev=0 (fresh)   → first sample for this pid (no prior basis);
            //                      a restarted process restarts its counter.
            //   interval<=0      → first tick overall (no wall-clock delta yet).
            if (cpu_trace_) {
                std::ostringstream os;
                os << "cpu " << w->name << " pid=" << w->pid
                   << " smoothed=" << (s.cpu_pct / 100.0) << "% raw="
                   << raw_pct << "%"
                   << " (delta=" << delta << "j cur=" << cur << "j prev=" << prev
                   << "j interval=" << interval_s << "s clk_tck=" << clk_tck
                   << (fresh ? " FRESH(no-basis)" : "")
                   << (delta == 0 ? " IDLE(delta=0)" : "")
                   << (interval_s <= 0.0 ? " NO-INTERVAL" : "") << ")";
                log_info(os.str());
            }
        } else if (cpu_trace_) {
            log_info("cpu " + w->name + " pid=" + std::to_string(w->pid) +
                     " cpu_pct=0 (read_proc_stat FAILED — /proc/<pid>/stat "
                     "unreadable; transient on a just-exited pid)");
        }
        uint64_t rss = 0, vsz = 0, data = 0;
        if (read_proc_status(w->pid, &rss, &vsz, &data)) {
            s.rss_kb  = rss;
            s.vsz_kb  = vsz;
            s.data_kb = data;       // 0 if VmData line absent (rare)
        }
        uint64_t shared = 0;
        if (read_proc_smaps_rollup_shared(w->pid, &shared)) {
            s.shared_kb = shared;
        }

        // Per-thread sample. Carry forward previous tick's per-tid
        // jiffies counter so cpu% has a proper delta basis. TIDs that
        // disappear (thread exited) get pruned by rebuilding the map.
        auto tids = list_tids(w->pid);
        std::map<uint32_t, ThreadEntry> next_threads;
        for (uint32_t tid : tids) {
            ThreadEntry te = s.threads_detail[tid];      // carry prev_jiffies
            te.tid = tid;
            uint64_t ut = 0, st = 0;
            int32_t  nc = 0;
            uint32_t lcpu = 0, rt = 0, pol = 0;
            if (!read_thread_stat(w->pid, tid, &ut, &st, &nc,
                                   &lcpu, &rt, &pol)) {
                // thread vanished mid-read; skip
                continue;
            }
            uint64_t cur = ut + st;
            uint64_t delta = (te.prev_jiffies <= cur) ? (cur - te.prev_jiffies) : 0;
            te.prev_jiffies = cur;
            if (interval_s > 0.0) {
                double pct = static_cast<double>(delta) /
                             (static_cast<double>(clk_tck) * interval_s) * 100.0;
                if (pct < 0.0)   pct = 0.0;
                if (pct > 6553.0) pct = 6553.0;
                te.cpu_pct = static_cast<uint32_t>(pct * 100.0 + 0.5);
            }
            te.nice           = nc;
            te.last_cpu       = lcpu;
            te.sched_priority = rt;
            te.sched_policy   = pol;
            (void)read_thread_comm(w->pid, tid, &te.comm);
            uint64_t mask = 0;
            (void)read_thread_affinity_mask(w->pid, tid, &mask);
            te.cpu_affinity_mask = mask;
            next_threads[tid] = te;
        }
        s.threads_detail = std::move(next_threads);

        // TIPC sockets: join this pid's open socket inodes against the
        // tick's TIPC diag table. Only TIPC sockets match (others aren't in
        // the table), so this naturally filters to the node's TIPC ports.
        s.sockets.clear();
        if (!tipc_by_inode.empty()) {
            for (uint64_t ino : list_pid_socket_inodes(w->pid)) {
                auto it = tipc_by_inode.find(ino);
                if (it != tipc_by_inode.end()) s.sockets.push_back(it->second);
            }
        }

        next[w->pid] = s;
    }
    sample_.swap(next);
}

void Supervisor::emit_snapshot() {
    // Refresh the /proc samples (cpu/rss/threads) the topo-pair stream reads,
    // then bump the generation and walk the tree as the topo-pair firehose.
    // The old monolithic libprotobuf TreeSnapshot + the TipcPublisher/etcd
    // publish were removed with the com/gui/etcd retirement — the firehose
    // (SnapshotBegin → {NodeEdge+NodeState} → SnapshotEnd, over the EmitSink)
    // is the sole snapshot path now.
    sample_procs();
    ++generation_;
    emit_tree_stream();
}

// ---------------------------------------------------------------------------
// Control-op helper implementations (find/start/delete/restart/terminate/...).
// ---------------------------------------------------------------------------

WorkerNode* Supervisor::find_worker_by_name(const std::string& name) {
    for (auto* w : all_workers(*root_)) {
        if (w->name == name) return w;
    }
    return nullptr;
}

// (find_node_by_name removed — node-name → address resolution moved into the
// immutable Registry, core/registry.*.)

SupervisorNode* Supervisor::find_supervisor_by_name(const std::string& name) {
    std::vector<SupervisorNode*> stack{root_};
    while (!stack.empty()) {
        SupervisorNode* s = stack.back();
        stack.pop_back();
        if (s->name == name) return s;
        for (auto& c : s->children) {
            if (!c->is_worker()) stack.push_back(&c->sup);
        }
    }
    return nullptr;
}

pid_t Supervisor::do_start_child(const std::string& parent_sup,
                                  const std::string& name,
                                  const std::vector<std::string>& start_cmd,
                                  int restart, int /*shutdown*/, int type,
                                  const std::vector<std::string>& modules,
                                  uint32_t& status) {
    if (name.empty() || start_cmd.empty()) {
        status = 4;     // invalid_request
        return -1;
    }
    if (find_worker_by_name(name) || find_supervisor_by_name(name)) {
        status = 5;     // already_present
        return -1;
    }
    SupervisorNode* parent = parent_sup.empty() ? root_
                                                 : find_supervisor_by_name(parent_sup);
    if (!parent) {
        status = 1;     // not_found
        return -1;
    }
    // Build the child Node. For now only worker children are
    // supported (type=0); supervisor-children would need a full nested
    // sub-tree and are deferred.
    if (type != 0) {
        status = 4;     // invalid_request
        return -1;
    }

    WorkerNode w;
    w.name      = name;
    w.start_cmd = start_cmd;
    w.modules   = modules;
    switch (restart) {
        case 1:  w.restart = RestartType::Transient; break;
        case 2:  w.restart = RestartType::Temporary; break;
        default: w.restart = RestartType::Permanent; break;
    }

    auto node = Node::make_worker(std::move(w));
    Node* node_ptr = node.get();
    parent->children.push_back(std::move(node));

    // Spawn immediately. OTP semantics: start_child returns once the
    // start function returns.
    start_worker(node_ptr->worker);
    status = (node_ptr->worker.pid > 0) ? 0 : 4;
    return node_ptr->worker.pid;
}

uint32_t Supervisor::do_delete_child(const std::string& name) {
    // Walk: for every supervisor, find a child with this name.
    std::vector<SupervisorNode*> stack{root_};
    while (!stack.empty()) {
        SupervisorNode* s = stack.back();
        stack.pop_back();
        for (auto it = s->children.begin(); it != s->children.end(); ++it) {
            Node* c = it->get();
            if (c->is_worker()) {
                if (c->worker.name != name) continue;
                if (c->worker.pid > 0) return 4;    // running — cannot delete
                s->children.erase(it);
                return 0;
            } else {
                if (c->sup.name == name) {
                    // Refuse to delete a supervisor with live children;
                    // the operator must terminate them first.
                    if (!c->sup.children.empty()) return 6;  // child_busy
                    s->children.erase(it);
                    return 0;
                }
                stack.push_back(&c->sup);
            }
        }
    }
    return 1;       // not_found
}

uint32_t Supervisor::do_restart_child(const std::string& name) {
    WorkerNode* w = find_worker_by_name(name);
    if (!w) {
        // The GUI/snapshot synthesizes a `<worker>_sup` BRACKET row for each
        // reporting worker (#364) — it's NOT a real SupervisorNode. "Restart
        // subtree" on that bracket means restart its one wrapped worker. Strip
        // the suffix and retry as a worker before falling through to a real sup.
        const std::string kSuf = "_sup";
        if (name.size() > kSuf.size() &&
            name.compare(name.size() - kSuf.size(), kSuf.size(), kSuf) == 0) {
            const std::string base = name.substr(0, name.size() - kSuf.size());
            if (find_worker_by_name(base))
                return do_restart_child(base);  // restart the wrapped worker
        }
        // Not a worker — maybe it's a REAL SUPERVISOR. "Restart subtree" on a
        // sup (GUI right-click) restarts every worker under it, OTP-style (stop
        // reverse, start forward). Without this the GUI's Restart-subtree was a
        // silent no-op (find_worker_by_name returned null → not_found).
        if (SupervisorNode* sup = find_supervisor_by_name(name)) {
            restart_all(*sup);
            // Count the restart on the supervisor itself + each restarted
            // worker, so the stat/firehose reflect it (mirrors do_restart_child
            // for a worker). restart_all already re-forked them.
            for (WorkerNode* cw : all_workers(*sup)) {
                if (cw->pid > 0) {
                    cw->restart_count++;
                    cast_node_state(*cw);
                }
            }
            sup->restart_count++;
            total_restarts_++;
            return 0;
        }
        return 1;     // not_found (neither worker nor supervisor)
    }
    if (w->pid > 0) {
        stop_worker(*w);
    }
    start_worker(*w);
    // A deliberate restart (tdb restart / GUI "Kill (restart)" → RestartChild)
    // is still a restart: count it so the stat tracks it. stop_worker() sets
    // terminating, so the SIGCHLD reap path skips on_child_exit (which bumps
    // the count for a CRASH) — without this the manual-restart count never
    // moves and the GUI/tdb show a stale 0. Bump worker + supervisor in
    // lockstep, then emit the NodeState so consumers (firehose) see the new
    // count + pid live, same as the crash path does.
    if (w->pid > 0) {
        if (SupervisorNode* sup = supervisor_of(*w)) {
            bump_restart_count_(*w, *sup);
        } else {
            w->restart_count++;   // orphan (shouldn't happen): count the worker
        }
        cast_node_state(*w);
    }
    return (w->pid > 0) ? 0 : 4;
}

uint32_t Supervisor::do_terminate_child(const std::string& name) {
    WorkerNode* w = find_worker_by_name(name);
    if (!w) return 1;     // not_found
    if (w->pid <= 0) return 3;     // already_stopped
    stop_worker(*w);
    return 0;
}

// SuspendChild — stop the process and HOLD it down (no restart, no watchdog
// escalation) so a probe can take over its node's TIPC address for mocking.
// Set `held` BEFORE stopping so the reap path treats this as a held stop.
uint32_t Supervisor::do_suspend_child(const std::string& name) {
    WorkerNode* w = find_worker_by_name(name);
    if (!w) return 1;     // not_found
    if (w->held) return 0;   // already held — idempotent
    w->held = true;
    if (w->pid > 0) stop_worker(*w);
    log_info("child " + w->name + " SUSPENDED (held; restart/watchdog off)");
    return 0;
}

// ResumeChild — clear the hold and restart the process. The freshly-spawned
// instance re-binds its TIPC address and re-arms the watchdog on first beat.
uint32_t Supervisor::do_resume_child(const std::string& name) {
    WorkerNode* w = find_worker_by_name(name);
    if (!w) return 1;     // not_found
    if (!w->held) return 3;   // not held
    w->held = false;
    start_worker(*w);
    log_info("child " + w->name + " RESUMED (restarted)");
    return (w->pid > 0) ? 0 : 4;
}


// ---------------------------------------------------------------------------
// SystemInfo — host facts from uname(2) + /proc + /etc/os-release.
// ---------------------------------------------------------------------------

namespace {

std::string slurp_text(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return {};
    std::string out;
    char buf[1024];
    while (true) {
        size_t n = std::fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        out.append(buf, n);
    }
    std::fclose(f);
    return out;
}

std::string parse_os_release_pretty(const std::string& body) {
    const char* k = "PRETTY_NAME=";
    size_t pos = body.find(k);
    if (pos == std::string::npos) return {};
    pos += std::strlen(k);
    if (pos < body.size() && body[pos] == '"') ++pos;
    size_t end = pos;
    while (end < body.size() && body[end] != '"' && body[end] != '\n') ++end;
    return body.substr(pos, end - pos);
}

uint64_t parse_mem_total_kb(const std::string& body) {
    size_t pos = body.find("MemTotal:");
    if (pos == std::string::npos) return 0;
    pos += std::strlen("MemTotal:");
    while (pos < body.size() && body[pos] == ' ') ++pos;
    char* end = nullptr;
    return std::strtoull(body.c_str() + pos, &end, 10);
}

uint64_t parse_uptime_sec(const std::string& body) {
    char* end = nullptr;
    double v = std::strtod(body.c_str(), &end);
    return v > 0 ? static_cast<uint64_t>(v) : 0;
}

// statvfs a path; fill total/avail kB (0/0 if the path can't be stat'd —
// e.g. an install dir that doesn't exist yet). Uses f_frsize (fragment size)
// for the byte conversion, f_blocks for total, f_bavail for free-to-unpriv.
void statvfs_kb(const char* path, uint64_t& total_kb, uint64_t& avail_kb) {
    struct statvfs vfs{};
    if (::statvfs(path, &vfs) != 0) { total_kb = avail_kb = 0; return; }
    const uint64_t unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    total_kb = (static_cast<uint64_t>(vfs.f_blocks) * unit) / 1024;
    avail_kb = (static_cast<uint64_t>(vfs.f_bavail) * unit) / 1024;
}

}  // namespace


void Supervisor::do_get_system_info(SystemInfoData& info) {
    struct utsname u{};
    if (::uname(&u) == 0) {
        info.hostname = u.nodename;
        std::string kernel;
        kernel.reserve(64);
        kernel.append(u.sysname).append(" ")
              .append(u.release).append(" ")
              .append(u.machine);
        info.kernel = std::move(kernel);
    }

    info.os_pretty_name =
        parse_os_release_pretty(slurp_text("/etc/os-release"));

    long cpus = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus > 0) info.cpu_count = static_cast<uint32_t>(cpus);

    info.total_ram_kb = parse_mem_total_kb(slurp_text("/proc/meminfo"));
    info.uptime_sec   = parse_uptime_sec  (slurp_text("/proc/uptime"));

    // Disk free (System tab "Resources" box): root and the install/run dir
    // (where binaries + tombstones live). root_dir_ is the supervisor's stage
    // dir; if it's empty (unstaged), fall back to the current dir.
    statvfs_kb("/", info.disk_root_total_kb, info.disk_root_avail_kb);
    statvfs_kb(root_dir_.empty() ? "." : root_dir_.c_str(),
               info.disk_install_total_kb, info.disk_install_avail_kb);

    // Build-time facts injected via -D... when the build wires them.
    // Defaults to empty when undefined, matching proto3 default.
#ifdef THEIA_GIT_SHA
    info.theia_git_sha = THEIA_GIT_SHA;
#endif
#ifdef THEIA_BUILD_TIMESTAMP
    info.build_timestamp = THEIA_BUILD_TIMESTAMP;
#endif

    // Wall-clock millis when this supervisor process started.
    // start_time_ is a steady_clock point, so we approximate by
    // measuring delta-since-now in milliseconds and subtracting
    // from the current wall-clock.
    using namespace std::chrono;
    auto now_wall = system_clock::now();
    auto elapsed  = duration_cast<milliseconds>(
                       steady_clock::now() - start_time_);
    auto started_wall = now_wall -
        duration_cast<system_clock::duration>(elapsed);
    info.start_timestamp_ms =
        duration_cast<milliseconds>(
            started_wall.time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Watchdog (phase 4).
//
// Convention: a worker is "watched" once it has sent at least one
// HeartbeatReport. Bash daemons / passive children that never report
// stay exempt. The grace period is hardcoded here (3 × the expected
// 1 Hz period, i.e. 3s without a beat = wedged). The manifest carries
// watchdog_max_missed on the supervisor's .art node; param-from-art
// plumbing is a follow-up.
// ---------------------------------------------------------------------------

void Supervisor::check_heartbeats() {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    constexpr milliseconds kMaxAge{3000};  // 3s = 3 missed beats at 1Hz

    // Drop entries for pids that aren't alive any more (avoids growing
    // unbounded as workers get restarted with new pids).
    auto alive = all_workers(*root_);
    std::map<pid_t, WorkerNode*> by_pid;
    for (auto* w : alive) {
        if (w->pid > 0) by_pid[w->pid] = w;
    }

    for (auto it = heartbeats_.begin(); it != heartbeats_.end(); ) {
        if (by_pid.find(it->first) == by_pid.end()) {
            it = heartbeats_.erase(it);
            continue;
        }
        WorkerNode* held_w = by_pid[it->first];
        if (held_w && held_w->held) {
            // HELD for test mocking — not expected to beat. Drop the entry so
            // it isn't watchdogged; ResumeChild restarts + re-arms.
            it = heartbeats_.erase(it);
            continue;
        }
        auto age = duration_cast<milliseconds>(now - it->second.last_seen);
        if (age > kMaxAge) {
            WorkerNode* w = by_pid[it->first];
            std::ostringstream msg;
            msg << "watchdog: " << w->name << " (pid=" << w->pid
                << ") missed heartbeats (last seen "
                << age.count() << " ms ago); SIGTERMing";
            log_warn(msg.str());
            // Surface as a supervision event with kind=8 (watchdog).
            emit_event(/*kind=*/8, w, supervisor_of(*w), 0, "", msg.str());
            // Kill — the normal SIGCHLD reap + restart strategy applies.
            ::kill(w->pid, SIGTERM);
            // Drop the entry now; the next heartbeat (from the
            // restarted instance, if it heartbeats at all) re-arms.
            it = heartbeats_.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// #361 — Trace config storage + per-NODE push helpers.
// ---------------------------------------------------------------------------

namespace {

// (The old [u16 tag][payload] send_frame_to_tipc_name helper was removed
// with the dead kTagTraceApplyConfig=0x0300 node push — trace config now
// rides the standard TraceControlPush GW_MSG_GEN_CAST via
// send_gw_cast_to_tipc_name below, #403.)

// ---- config push: STRUCTURED INTENT, no wire knowledge --------------------
//
// The supervisor pushes trace/log control to a child's config-service receiver
// (a runtime TipcMux on the child). The ENGINE owns the config STATE + resolves
// the child's (type,instance), then emits the typed values via
// EmitSink.on_trace_push / on_log_push. It does NOT encode protobuf, compute a
// service_id, or open a socket. The FC shell's runnable (which links
// platform/runtime) builds the real platform_runtime_TraceControlPush /
// LogLevelPush and casts it via RemoteCodec — service_id + wire bytes match the
// child's register_cast<Msg> by construction. (The old hand encode_*_push +
// djb2_low16 + GwHdrWire send_gw_cast_to_tipc_name were all deleted — no nanopb,
// no transport, no wire knowledge in this TU.)

// (The hand proto3-wire firehose encoders + the com ComDaemon cast target
// were removed with the com retirement. The topo-pair firehose now leaves
// via the EmitSink as plain EdgeData/NodeStateData structs; SupervisorCtl's
// `events` broadcast senders do the nanopb encode + TIPC fan-out.)

// Map a LogLevelValue ORDINAL (0..4) → the level NAME the child reads from its
// THEIA_LOG_LEVEL spawn env at boot. The ONLY ordinal↔string conversion left:
// the live push forwards the ordinal verbatim; only the restart env needs a
// name. Thin shim over platform/runtime's log_level_name(LogLevel) — the
// LogLevelValue ordinals are aligned with ::theia::runtime::LogLevel
// (Trace=0..Error=4), so this is just the ordinal→enum cast (out-of-range falls
// through to the runtime's "info" default, same as before).
const char* log_level_name(uint32_t level) {
    return ::theia::runtime::log_level_name(
        static_cast<::theia::runtime::LogLevel>(level));
}

}  // namespace

// ---- #429/#430 topo-pair firehose emit (now over the EmitSink) -----------

// Build the NodeStateData for a worker from its current runtime state + the
// last /proc sample (filled by sample_procs() in emit_snapshot). pid doubles
// as the primary thread id (the main thread's tid == pid on Linux), which is
// what changes on restart.
void Supervisor::cast_node_state(const WorkerNode& w) {
    if (!emit_.on_node_state) return;
    NodeStateData ns;
    ns.name           = w.name;
    ns.pid            = w.pid;
    ns.tid            = (w.pid > 0) ? static_cast<uint32_t>(w.pid) : 0;
    ns.state          = (w.pid > 0) ? 2u : 0u;   // running / stopped
    if (w.terminating) ns.state = 3u;            // terminating
    ns.flags          = w.flags;
    ns.restart_count  = w.restart_count;
    ns.last_exit_code = w.last_exit_code;
    if (w.pid > 0) {
        ns.uptime_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - w.last_start).count());
        auto it = sample_.find(w.pid);
        if (it != sample_.end()) {
            ns.cpu_pct   = it->second.cpu_pct;
            ns.rss_kb    = it->second.rss_kb;
            ns.vsz_kb    = it->second.vsz_kb;
            ns.threads   = it->second.threads;
            ns.shared_kb = it->second.shared_kb;
            ns.data_kb   = it->second.data_kb;
        }
    }
    emit_.on_node_state(ns);
}

void Supervisor::emit_tree_stream() {
    // SnapshotBegin opens the walk under the CURRENT generation (already
    // bumped by emit_snapshot before this call).
    if (emit_.on_snapshot_begin) emit_.on_snapshot_begin(generation_, epoch_ms());

    auto edge = [&](uint32_t op, const std::string& parent,
                    const std::string& name, uint32_t kind) {
        if (emit_.on_edge) emit_.on_edge(EdgeData{op, parent, name, kind});
    };
    auto sup_state = [&](const std::string& name, uint32_t restart_count) {
        if (!emit_.on_node_state) return;
        NodeStateData ns;
        ns.name          = name;
        ns.pid           = -1;
        ns.state         = 2;  // running (a supervisor is "up" if walked)
        ns.restart_count = restart_count;
        emit_.on_node_state(ns);
    };

    // Topological walk — identical shape to emit_snapshot's: root, then each
    // child under its (already-emitted) parent. For each node: NodeEdge(ADD)
    // then NodeState. Synthesize the same <worker>_sup bracket row (#364) so
    // a rebuilt tree matches the legacy hierarchy.
    std::function<void(const SupervisorNode&, const std::string&)> walk =
        [&](const SupervisorNode& sup, const std::string& parent) {
            edge(/*ADD*/0, parent, sup.name, /*kind sup*/1);
            sup_state(sup.name, sup.restart_count);  // cumulative, not window

            for (const auto& c : sup.children) {
                if (c->is_worker()) {
                    std::string worker_parent = sup.name;
                    bool has_reporting = false;
                    for (const auto& ni : c->worker.nodes) {
                        if (ni.reporting) { has_reporting = true; break; }
                    }
                    if (has_reporting) {
                        const std::string synth = c->worker.name + "_sup";
                        edge(0, sup.name, synth, 1);
                        // Bracket wraps one worker — mirror its cumulative count.
                        sup_state(synth, c->worker.restart_count);
                        worker_parent = synth;
                    }
                    // worker edge (kind=0) + its NodeState.
                    edge(0, worker_parent, c->worker.name, 0);
                    cast_node_state(c->worker);
                } else {
                    walk(c->sup, sup.name);
                }
            }
        };
    walk(*root_, "");

    if (emit_.on_snapshot_end) emit_.on_snapshot_end(generation_);
}

void Supervisor::apply_trace_config(const std::string& target_node,
                                    uint32_t kind,
                                    bool enabled) {
    if (target_node.empty()) {
        log_warn("apply_trace_config: empty target_node — dropping");
        return;
    }
    auto& by_kind = trace_configs_[target_node];
    if (enabled) {
        if (kind == 0) {
            // Catch-all ("all kinds"): REPLACE the set with just kind 0. The
            // node wipes its mask on a kind-0 enable (mask==0 → all pass), so a
            // mix of stale narrow kinds would misrepresent its state on re-push.
            by_kind.clear();
            by_kind[0] = true;
        } else {
            // A specific kind narrows. If the catch-all was set, drop it — the
            // set now means "exactly these kinds", matching the node's mask.
            by_kind.erase(0);
            by_kind[kind] = true;
        }
    } else {
        if (kind == 0) {
            by_kind.clear();   // disable kind 0 = stop the whole child
        } else {
            by_kind.erase(kind);
        }
        if (by_kind.empty()) trace_configs_.erase(target_node);
    }
    log_info(std::string("trace config ") + (enabled ? "ENABLE" : "DISABLE") +
             " kind " + std::to_string(kind) + " for " + target_node);
    // STORE ONLY. The LIVE cast is done by SupervisorCtl after ctl_configure_*
    // returns. Restart re-push still routes through the heartbeat-after-gap path
    // (push_*_to_child → on_*_push → SupervisorCtl forwarder cast).
}

// (resolve_trace_target removed — name→address resolution now lives in the
// immutable Registry (core/registry.*), built once from the manifest. The old
// tree-walking resolver is gone; SupervisorCtl resolves off registry() and the
// configure-* validation does too. The Registry encodes the SAME worker-name →
// first-reporting-node / node-name → own-address rules.)

// (push_trace_disable_to_child removed — SupervisorCtl now casts the disable
// directly from ConfigureTrace's handler, the same plain resolve+cast path as
// enable.)

void Supervisor::push_trace_config_to_child(const std::string& child_name) {
    auto cfg_it = trace_configs_.find(child_name);
    if (cfg_it == trace_configs_.end()) return;  // nothing to push
    if (!emit_.set_trace) return;

    // Ask CONTROL to set each enabled kind on the child BY NAME. The engine does
    // NOT resolve the address or cast — SupervisorCtl::set_trace does both. One
    // push per enabled kind so the node rebuilds its full mask. Inner key =
    // TraceKind ordinal; value = enabled.
    int pushed = 0;
    for (const auto& kv : cfg_it->second) {
        if (!kv.second) continue;
        emit_.set_trace(child_name, /*kind=*/kv.first, /*enabled=*/true);
        ++pushed;
    }
    log_debug("asked control to set " + std::to_string(pushed) +
              " trace entries on '" + child_name + "'");
}

// ---- #385: per-child log level -------------------------------------------

void Supervisor::apply_log_level(const std::string& target_node,
                                 uint32_t level) {
    if (target_node.empty()) {
        log_warn("apply_log_level: empty target_node — dropping");
        return;
    }
    log_levels_[target_node] = level;  // store the ordinal verbatim

    // Rewrite the worker's spawn env so a (re)start boots at the new level
    // (main.cc reads THEIA_LOG_LEVEL — a NAME — once at startup). This is the
    // one place the ordinal becomes a string; the live push forwards the
    // ordinal as-is.
    WorkerNode* w = find_worker_by_name(target_node);
    if (w) {
        w->env["THEIA_LOG_LEVEL"] = log_level_name(level);
    }

    log_info(std::string("log level for ") + target_node + " -> " +
             log_level_name(level));
    // STORE ONLY. SupervisorCtl casts the live update after ctl_configure_*
    // returns; the heartbeat-after-gap path re-pushes on restart.
}

void Supervisor::push_log_level_to_child(const std::string& child_name) {
    auto it = log_levels_.find(child_name);
    if (it == log_levels_.end() || it->second == kNoLevel) return;  // nothing to push
    if (!emit_.set_log_level) return;
    // Ask CONTROL to set the stored level on the child BY NAME — SupervisorCtl
    // resolves the address + casts LogLevelPush. The engine doesn't resolve.
    emit_.set_log_level(child_name, it->second);
    log_debug(std::string("asked control to set log level '") +
              log_level_name(it->second) + "' on '" + child_name + "'");
}

// (send_sm_ready removed — the SM startup handshake is not part of this e2e
// and is being re-homed. The supervisor no longer casts SystemBoot/
// StartupComplete to the sm gate; whoever owns boot sequencing drives it.)

}  // namespace supervisor

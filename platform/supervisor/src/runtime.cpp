// Supervisor runtime — fork/exec children, observe exits, apply OTP-style
// restart strategy. Mirrors supervisor/runtime.py.

#include "supervisor/runtime.h"

// Generated from services/supervisor/system/package.art by
// `artheia gen-proto` and compiled by protoc via the CMake build.
#include "ChildSelector.pb.h"
#include "ChildSpec.pb.h"
#include "ChildState.pb.h"
#include "ControlReply.pb.h"
#include "ControlRequest.pb.h"
#include "DeleteChildRequest.pb.h"
#include "GetChildRequest.pb.h"
#include "GetSystemInfoRequest.pb.h"
#include "GetTreeRequest.pb.h"
#include "SystemInfo.pb.h"
#include "HealthBeacon.pb.h"
#include "HeartbeatReport.pb.h"
#include "SendTimeoutReport.pb.h"
#include "StartChildRequest.pb.h"
#include "SupervisionEvent.pb.h"
#include "TraceConfig.pb.h"
#include "TraceConfigList.pb.h"
#include "ConfigureTraceRequest.pb.h"
#include "GetTraceConfigRequest.pb.h"
#include "TreeSnapshot.pb.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/utsname.h>
#include <dirent.h>
#include <functional>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <sched.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <linux/tipc.h>
#include <sys/socket.h>
#include <unistd.h>

namespace supervisor {

namespace {

void log_line(const std::string& level, const std::string& msg) {
    // ISO-8601 timestamp + level + message. stderr so children's stdout
    // stays unmangled.
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&tt));
    std::fprintf(stderr, "%s %s supervisor: %s\n", buf, level.c_str(), msg.c_str());
}

void log_info(const std::string& m)  { log_line("INFO", m); }
void log_warn(const std::string& m)  { log_line("WARN", m); }
void log_err (const std::string& m)  { log_line("ERROR", m); }

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

// Locate the most recent tombstone matching <dir>/tombstone-<name>-<pid>-*.
// Returns "" if no match. We pick the newest mtime to handle multi-run
// noise.
std::string locate_tombstone(const std::string& dir,
                             const std::string& name,
                             pid_t pid) {
    DIR* d = opendir(dir.c_str());
    if (!d) return {};
    std::string prefix = "tombstone-" + name + "-" + std::to_string(pid) + "-";
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

}  // namespace

Supervisor::Supervisor(std::unique_ptr<Node> root,
                        std::string root_dir,
                        std::string etcd_endpoints,
                        std::string machine_name)
    : root_node_(std::move(root)), root_dir_(std::move(root_dir)) {
    if (!root_node_ || !root_node_->is_supervisor()) {
        throw std::runtime_error("root must be a supervisor");
    }
    root_ = &root_node_->sup;

    start_time_      = std::chrono::steady_clock::now();
    last_heartbeat_  = start_time_;
    last_snapshot_   = start_time_;

    // Bind the supervisor's TIPC service. .art declares this address —
    // hardcoded here until the param-from-manifest plumbing lands.
    // Best-effort: if the kernel lacks AF_TIPC (no module / no perms)
    // the supervisor still runs; just no GUI / com feed.
    publisher_.open(0x80020001, 0);

    // Inbound dispatch lambda captures `this` for routing. Phases 3+4
    // add real handler bodies; today every tag is a quiet drop.
    publisher_.set_inbound_callback(
        [this](int fd, uint16_t tag, const std::string& payload) {
            on_inbound_frame(fd, tag, payload);
        });

    // Resolve a machine_name for etcd keying. Priority:
    //   1. explicit arg (from main.cpp via --machine-name or env)
    //   2. $HOSTNAME (docker compose sets this; Linux usually has it)
    //   3. gethostname(2)
    if (machine_name.empty()) {
        const char* h = std::getenv("HOSTNAME");
        if (h && *h) {
            machine_name = h;
        } else {
            char buf[256] = {};
            if (gethostname(buf, sizeof(buf) - 1) == 0) {
                machine_name = buf;
            } else {
                machine_name = "unknown";
            }
        }
    }

    // Open the etcd publisher. If endpoints is empty, this is a no-op:
    // every publish_*() call below stays a no-op and we run with TIPC
    // only — the existing single-machine dev flow.
    etcd_publisher_.open(etcd_endpoints, machine_name);

    // Block SIGCHLD/SIGTERM/SIGINT process-wide; we read them via signalfd.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
        throw std::runtime_error(std::string("sigprocmask: ") + std::strerror(errno));
    }
    signal_fd_ = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (signal_fd_ < 0) {
        throw std::runtime_error(std::string("signalfd: ") + std::strerror(errno));
    }
}

Supervisor::~Supervisor() {
    if (signal_fd_ >= 0) close(signal_fd_);
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
    emit_event(/*kind=child_started*/0, &w, supervisor_of(w),
               /*exit_code=*/0, std::string{}, std::string{});
    emit_snapshot();
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
            }
        }
    }

    if (w.restart == RestartType::Temporary)             return;
    if (w.restart == RestartType::Transient && !abnormal) return;

    if (!record_and_check_restart(*sup)) {
        std::ostringstream msg;
        msg << "supervisor " << sup->name << " exceeded restart intensity ("
            << sup->max_restarts << " in " << sup->max_seconds
            << "s) — escalating";
        log_err(msg.str());
        escalated_ = true;
        shutdown_requested_.store(true);
        return;
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

int Supervisor::run() {
    log_info("supervisor starting (root=" + root_->name + ")");
    start_subtree(*root_);

    // T1: arm the SM startup handshake. Children were just forked; give
    // them a grace window to bind their TIPC sockets, then send_sm_ready()
    // (below, in the loop) casts SystemBoot + StartupComplete to sm.
    sm_ready_deadline_ =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);

    while (!shutdown_requested_.load()) {
        // Wait for signalfd readable; budget 1s so we wake periodically.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(signal_fd_, &rfds);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int rv = ::select(signal_fd_ + 1, &rfds, nullptr, nullptr, &tv);
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

                switch (si.ssi_signo) {
                    case SIGTERM:
                    case SIGINT:
                        log_info("shutdown requested (signal=" +
                                 std::to_string(si.ssi_signo) + ")");
                        shutdown_requested_.store(true);
                        break;
                    case SIGCHLD:
                        // Handled by reap() below; nothing else to do here.
                        break;
                    default:
                        break;
                }
            }
        }

        // Reap any exited workers, regardless of whether select returned a
        // signalfd event — we may have missed coalesced SIGCHLDs.
        reap();

        // T1: once the post-boot grace window elapses, send the SM
        // startup handshake exactly once (children have had time to bind
        // their TIPC sockets).
        if (!sm_ready_sent_ &&
            std::chrono::steady_clock::now() >= sm_ready_deadline_) {
            send_sm_ready();
            sm_ready_sent_ = true;
        }

        // Drain the TIPC socket: accept new clients, deliver inbound
        // ControlRequest / HeartbeatReport / SendTimeoutReport frames
        // to on_inbound_frame() via the installed callback.
        publisher_.poll();

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
            check_heartbeats();
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

        if (was_terminating) {
            // stop_worker() owns this exit; just acknowledge and move on.
            log_info("child " + w.name + " stopped (terminating path)");
            continue;
        }
        on_child_exit(w, rc, old_pid);
    }
}

// ---------------------------------------------------------------------------
// Event emission helpers — protobuf wire, schema from services.supervisor.
// ---------------------------------------------------------------------------

namespace {
// Framing tags — see platform/supervisor/system/package.art docstring
// for the canonical schema.
constexpr uint16_t kTagEvent          = 0x0001;
constexpr uint16_t kTagHealth         = 0x0002;
constexpr uint16_t kTagSnapshot       = 0x0003;
constexpr uint16_t kTagSystemInfo     = 0x0004;  // reply to GetSystemInfo
// Inbound tags (filled by phase 3 / phase 4):
constexpr uint16_t kTagControlRequest = 0x0100;
constexpr uint16_t kTagControlReply   = 0x0101;
constexpr uint16_t kTagHeartbeat      = 0x0200;
constexpr uint16_t kTagSendTimeout    = 0x0201;
}  // namespace

// op_kind values from the .art ControlRequest envelope.
namespace {
constexpr uint32_t kOpGetTree         = 1;
constexpr uint32_t kOpGetChild        = 2;
constexpr uint32_t kOpStartChild      = 3;
constexpr uint32_t kOpDeleteChild     = 4;
constexpr uint32_t kOpRestartChild    = 5;
constexpr uint32_t kOpTerminateChild  = 6;
constexpr uint32_t kOpStop            = 7;
constexpr uint32_t kOpGetSystemInfo   = 8;
constexpr uint32_t kOpConfigureTrace    = 9;
constexpr uint32_t kOpGetTraceConfig    = 10;
constexpr uint32_t kOpConfigureLogLevel = 11;

// Per-node NodeTraceCtl tag — what we send out the wire to a worker
// when pushing an ApplyConfig. The runtime side (gen-app emitted
// NodeTraceCtl receiver, #363) decodes a TraceConfig from the
// payload and applies it via Tracer::trace_enable (#355).
constexpr uint16_t kTagTraceApplyConfig = 0x0300;

// (#385's bespoke kTagLogApplyConfig=0x0400 tag is retired: the log-level
// push now rides a standard GW_MSG_GEN_CAST frame, decoded by the FC's
// normal runtime TipcMux. See send_gw_cast_to_tipc_name + #386. Trace
// (0x0300) still uses the legacy frame until its FC receiver moves to the
// same path.)

// "Stale heartbeat" gap: if the last heartbeat we saw from a child
// is older than this, treat the NEXT heartbeat as a (re)start
// trigger and re-push the saved trace config. Tuned to be
// comfortably larger than the runtime's default heartbeat period
// (1s); a 5s gap means "process was almost certainly restarted".
constexpr auto kHeartbeatGapForRepush = std::chrono::seconds(5);
}  // namespace

void Supervisor::on_inbound_frame(int client_fd, uint16_t tag,
                                  const std::string& payload) {
    if (tag == kTagControlRequest) {
        ::services::supervisor::ControlRequest req;
        if (!req.ParseFromString(payload)) {
            log_warn("ControlRequest malformed; dropping frame");
            return;
        }
        ::services::supervisor::ControlReply rep;
        rep.set_correlation_id(req.correlation_id());

        switch (req.op_kind()) {
            case kOpGetTree:
                // GetTree returns a TreeSnapshot, not a ControlReply.
                // Reply with the snapshot directly (tag 0x0003); the
                // client sees correlation_id absent and treats that as
                // a TreeSnapshot-shaped reply. (Pragmatic choice;
                // alternative is a oneof on ControlReply.)
                publisher_.reply_to(client_fd, kTagSnapshot,
                                    /*serialised*/ [&]() {
                                        ::services::supervisor::TreeSnapshot snap;
                                        // Re-use emit_snapshot's body via a
                                        // refactored helper: simplest is to
                                        // call emit_snapshot() which broadcasts
                                        // — duplicate is acceptable today.
                                        emit_snapshot();
                                        return std::string{};
                                    }());
                rep.set_status(0);
                publisher_.reply_to(client_fd, kTagControlReply,
                                    rep.SerializeAsString());
                return;

            case kOpGetChild: {
                const auto& name = req.get_child().name();
                WorkerNode* w = find_worker_by_name(name);
                if (!w) {
                    rep.set_status(1);     // not_found
                    rep.set_message("no such child");
                } else {
                    rep.set_status(0);
                    rep.set_child_name(name);
                }
                break;
            }

            case kOpStartChild: {
                const auto& spec = req.start_child().spec();
                std::vector<std::string> cmd(spec.start_cmd().begin(),
                                              spec.start_cmd().end());
                std::vector<std::string> mods(spec.modules().begin(),
                                               spec.modules().end());
                uint32_t status = 0;
                do_start_child(spec.parent_supervisor(), spec.name(),
                                cmd, spec.restart(), spec.shutdown(),
                                spec.type(), mods, status);
                rep.set_status(status);
                rep.set_child_name(spec.name());
                break;
            }

            case kOpDeleteChild:
                rep.set_status(do_delete_child(req.delete_child().name()));
                rep.set_child_name(req.delete_child().name());
                break;

            case kOpRestartChild:
                rep.set_status(do_restart_child(req.restart_child().name()));
                rep.set_child_name(req.restart_child().name());
                break;

            case kOpTerminateChild:
                rep.set_status(do_terminate_child(req.terminate_child().name()));
                rep.set_child_name(req.terminate_child().name());
                break;

            case kOpStop:
                request_shutdown();
                rep.set_status(0);
                break;

            case kOpGetSystemInfo: {
                // Reply with a SystemInfo payload first (tag
                // kTagSystemInfo), then a status=0 ControlReply
                // for symmetry. The two messages carry the same
                // correlation_id; services/com pairs by tag.
                ::services::supervisor::SystemInfo info;
                do_get_system_info(info);
                publisher_.reply_to(client_fd, kTagSystemInfo,
                                     info.SerializeAsString());
                rep.set_status(0);
                break;
            }

            case kOpConfigureTrace: {
                // #361: store + push. The wire-side handler in
                // apply_trace_config does both. We do NOT validate
                // that target_node actually exists — the supervisor
                // is happy to store config for a child that hasn't
                // started yet (matches the "config survives restart"
                // contract).
                const auto& cfg = req.configure_trace().config();
                apply_trace_config(cfg.target_node(), cfg.msg_type(),
                                   cfg.enabled(), cfg.kind());
                rep.set_status(0);
                rep.set_message("trace config applied");
                break;
            }

            case kOpGetTraceConfig: {
                // Reply with TraceConfigList (tag kTagTraceConfig) +
                // a status=0 ControlReply, matching the GetSystemInfo
                // two-frame pattern. Flatten the per-child map into
                // a flat list — the client doesn't care which
                // supervisor key produced which entry.
                ::services::supervisor::TraceConfigList list;
                for (const auto& outer : trace_configs_) {
                    for (const auto& inner : outer.second) {
                        auto* c = list.add_configs();
                        c->set_target_node(outer.first);
                        c->set_msg_type(inner.first);
                        c->set_enabled(true);          // presence = enabled
                        c->set_kind(inner.second);     // value = TraceKind (#403)
                    }
                }
                publisher_.reply_to(client_fd, kTagTraceApplyConfig,
                                     list.SerializeAsString());
                rep.set_status(0);
                break;
            }

            case kOpConfigureLogLevel: {
                // #385: store + push, same shape as ConfigureTrace.
                // apply_log_level updates the in-memory table, rewrites
                // the worker's spawn-env THEIA_LOG_LEVEL (so a restart
                // re-applies), and pushes a LogLevelConfig frame live.
                // No target_node existence check — config for a
                // not-yet-started child is fine (survives-restart
                // contract).
                const auto& cfg = req.configure_log_level().config();
                apply_log_level(cfg.target_node(), cfg.level());
                rep.set_status(0);
                rep.set_message("log level applied");
                break;
            }

            default:
                rep.set_status(4);    // invalid_request
                rep.set_message("unknown op_kind");
                break;
        }
        publisher_.reply_to(client_fd, kTagControlReply,
                            rep.SerializeAsString());
        return;
    }

    if (tag == kTagHeartbeat) {
        ::services::supervisor::HeartbeatReport hb;
        if (!hb.ParseFromString(payload)) {
            log_warn("HeartbeatReport malformed");
            return;
        }
        pid_t pid = hb.pid();
        auto now = std::chrono::steady_clock::now();
        auto& s   = heartbeats_[pid];
        s.last_seen = now;
        s.last_seq = hb.seq();
        std::fprintf(stderr,
                     "supervisor: heartbeat node=%s pid=%d seq=%llu\n",
                     hb.node_name().c_str(), pid,
                     static_cast<unsigned long long>(hb.seq()));

        // #361 — "first heartbeat after a gap" is the re-push trigger
        // for the worker's saved trace config. Per-child tracking
        // (not per-pid) so the gap detection survives restart: a
        // freshly-spawned worker has no prior entry → treated as a
        // gap → push fires. This is also what handles the cold-start
        // case where ConfigureTrace arrived before the worker booted.
        const std::string& child = hb.node_name();
        auto it = last_heartbeat_by_child_.find(child);
        bool fire_push = (it == last_heartbeat_by_child_.end()) ||
                         (now - it->second > kHeartbeatGapForRepush);
        last_heartbeat_by_child_[child] = now;
        if (fire_push) {
            push_trace_config_to_child(child);
            // #385 — re-push the saved log level on the same
            // first-heartbeat-after-gap trigger, so a level set while
            // the child was down (or before it booted) lands once it
            // comes up.
            push_log_level_to_child(child);
        }
        return;
    }
    if (tag == kTagSendTimeout) {
        ::services::supervisor::SendTimeoutReport rep;
        if (!rep.ParseFromString(payload)) {
            log_warn("SendTimeoutReport malformed");
            return;
        }
        // Surface as a supervision event; the GUI Trace tab renders it
        // alongside crashes. Use the supervisor's "send_timeout" kind=7
        // (one past tree_changed); kind names live in the supervisor-gui
        // panels/trace_panel.cpp table.
        std::ostringstream detail;
        detail << rep.caller_node() << " → " << rep.callee_node()
               << " " << rep.iface() << "." << rep.method()
               << " budget=" << rep.budget_ms()
               << "ms observed=" << rep.observed_ms() << "ms";
        emit_event(/*kind=*/7, /*worker=*/nullptr, /*sup=*/nullptr,
                   /*exit_code=*/0, /*tombstone=*/"", detail.str());
        return;
    }

    std::fprintf(stderr, "supervisor: unknown inbound tag 0x%04x "
                 "(%zu bytes)\n", tag, payload.size());
}

void Supervisor::emit_event(uint32_t kind,
                            const WorkerNode* worker,
                            const SupervisorNode* sup,
                            int exit_code,
                            const std::string& tombstone_path,
                            const std::string& detail) {
    ::services::supervisor::SupervisionEvent ev;
    ev.set_kind(kind);
    ev.set_timestamp_ms(epoch_ms());
    if (worker) ev.set_child_name(worker->name);
    if (sup)    ev.set_supervisor_name(sup->name);
    ev.set_pid(worker ? worker->pid : -1);
    ev.set_exit_code(exit_code);
    if (sup) ev.set_strategy(to_string(sup->strategy));
    ev.set_tombstone_path(tombstone_path);
    ev.set_detail(detail);
    publisher_.publish(kTagEvent, ev.SerializeAsString());
    etcd_publisher_.publish_event(ev);
}

void Supervisor::emit_health() {
    using namespace std::chrono;
    const uint64_t uptime_ms =
        duration_cast<milliseconds>(steady_clock::now() - start_time_).count();

    uint32_t total = 0, active = 0;
    for (auto* w : all_workers(*root_)) {
        ++total;
        if (w->pid > 0) ++active;
    }

    ::services::supervisor::HealthBeacon hb;
    hb.set_timestamp_ms(epoch_ms());
    hb.set_uptime_ms(uptime_ms);
    hb.set_generation(generation_);
    hb.set_total_workers(total);
    hb.set_active_workers(active);
    hb.set_total_restarts(total_restarts_);
    hb.set_total_tombstones(total_tombstones_);
    publisher_.publish(kTagHealth, hb.SerializeAsString());
    etcd_publisher_.publish_health(hb);
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

    // Build the set of live pids; drop the rest from the map.
    auto workers = all_workers(*root_);
    std::map<pid_t, ProcSample> next;
    for (auto* w : workers) {
        if (w->pid <= 0) continue;
        ProcSample s = sample_[w->pid];   // carry forward prev_jiffies
        uint64_t ut = 0, st = 0;
        uint32_t th = 0;
        if (read_proc_stat(w->pid, &ut, &st, &th)) {
            uint64_t cur = ut + st;
            uint64_t delta = (s.prev_jiffies <= cur) ? (cur - s.prev_jiffies) : 0;
            s.prev_jiffies = cur;
            s.threads      = th;
            if (interval_s > 0.0) {
                // jiffies per second = clk_tck. CPU% = delta / (clk_tck * interval) * 100
                double pct = static_cast<double>(delta) /
                             (static_cast<double>(clk_tck) * interval_s) * 100.0;
                if (pct < 0.0)   pct = 0.0;
                if (pct > 6553.0) pct = 6553.0;  // fits in u32 ×100
                s.cpu_pct = static_cast<uint32_t>(pct * 100.0 + 0.5);
            }
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

        next[w->pid] = s;
    }
    sample_.swap(next);
}

void Supervisor::emit_snapshot() {
    sample_procs();
    ++generation_;

    ::services::supervisor::TreeSnapshot snap;
    snap.set_generation(generation_);
    snap.set_timestamp_ms(epoch_ms());

    std::function<void(const SupervisorNode&, const std::string&)> walk =
        [&](const SupervisorNode& sup, const std::string& parent) {
            if (!parent.empty()) {
                auto* row = snap.add_children();
                row->set_name(sup.name);
                row->set_parent_name(parent);
                row->set_kind(1);
                row->set_pid(-1);
                row->set_state(2);
                row->set_restart_count(
                    static_cast<uint32_t>(sup.restart_history.size()));
                row->set_strategy(to_string(sup.strategy));
                row->set_max_restarts(static_cast<uint32_t>(sup.max_restarts));
                row->set_max_seconds(static_cast<uint32_t>(sup.max_seconds));
            }
            for (const auto& c : sup.children) {
                if (c->is_worker()) {
                    std::string cmd;
                    for (size_t i = 0; i < c->worker.start_cmd.size(); ++i) {
                        if (i) cmd += ' ';
                        cmd += c->worker.start_cmd[i];
                    }
                    uint32_t state = (c->worker.pid > 0) ? 2 : 0;
                    if (c->worker.terminating) state = 3;

                    // #364 — synthesize a `<worker>_sup [one_for_all]` row
                    // between the worker and its parent when the worker
                    // has ≥1 reporting=true node. The GUI hangs the
                    // per-node tracing UI (#365) off this synthetic row
                    // — a node-thread death triggering one_for_all means
                    // the whole worker process is killed, matching the
                    // architectural contract from #366.
                    bool has_reporting = false;
                    for (const auto& ni : c->worker.nodes) {
                        if (ni.reporting) { has_reporting = true; break; }
                    }
                    std::string worker_parent = sup.name;
                    if (has_reporting) {
                        auto* synth = snap.add_children();
                        synth->set_name(c->worker.name + "_sup");
                        synth->set_parent_name(sup.name);
                        synth->set_kind(1);  // supervisor
                        synth->set_pid(-1);
                        synth->set_state(2);  // running
                        synth->set_restart_count(0);
                        synth->set_strategy("one_for_all");
                        // max_restarts/max_seconds default 0 — the synthetic
                        // row is a UI bracket, not a restart-budget owner;
                        // the parent's strategy still drives the actual
                        // cascade when the worker dies. GUI may display
                        // these as blanks.
                        worker_parent = synth->name();
                    }

                    auto* row = snap.add_children();
                    row->set_name(c->worker.name);
                    row->set_parent_name(worker_parent);
                    row->set_kind(0);
                    row->set_pid(c->worker.pid);
                    row->set_state(state);
                    row->set_restart_count(0);
                    row->set_start_cmd(cmd);
                    // Resource samples — taken in sample_procs() above.
                    if (c->worker.pid > 0) {
                        auto it = sample_.find(c->worker.pid);
                        if (it != sample_.end()) {
                            row->set_cpu_pct  (it->second.cpu_pct);
                            row->set_rss_kb   (it->second.rss_kb);
                            row->set_vsz_kb   (it->second.vsz_kb);
                            row->set_threads  (it->second.threads);
                            row->set_shared_kb(it->second.shared_kb);
                            row->set_data_kb  (it->second.data_kb);
                            // Per-thread detail. One entry per TID.
                            for (const auto& kv : it->second.threads_detail) {
                                const auto& te = kv.second;
                                auto* ts = row->add_threads_detail();
                                ts->set_tid              (te.tid);
                                ts->set_comm             (te.comm);
                                ts->set_cpu_pct          (te.cpu_pct);
                                ts->set_sched_policy     (te.sched_policy);
                                ts->set_sched_priority   (te.sched_priority);
                                ts->set_nice             (te.nice);
                                ts->set_cpu_affinity_mask(te.cpu_affinity_mask);
                                ts->set_last_cpu         (te.last_cpu);
                            }
                        }
                    }
                } else {
                    walk(c->sup, sup.name);
                }
            }
        };
    walk(*root_, "");

    publisher_.publish(kTagSnapshot, snap.SerializeAsString());
    etcd_publisher_.publish_tree(snap);

    // Also publish every child row individually so the GUI can
    // wxListCtrl on /theia/state/<m>/child/ instead of decoding the
    // whole TreeSnapshot. Same data, sliced for cheap prefix-Range +
    // per-child Watch.
    for (const auto& ch : snap.children()) {
        etcd_publisher_.publish_child(ch);
    }
}

// ---------------------------------------------------------------------------
// ControlRequest helper implementations (phase 3).
// ---------------------------------------------------------------------------

WorkerNode* Supervisor::find_worker_by_name(const std::string& name) {
    for (auto* w : all_workers(*root_)) {
        if (w->name == name) return w;
    }
    return nullptr;
}

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
    if (!w) return 1;     // not_found
    if (w->pid > 0) {
        stop_worker(*w);
    }
    start_worker(*w);
    return (w->pid > 0) ? 0 : 4;
}

uint32_t Supervisor::do_terminate_child(const std::string& name) {
    WorkerNode* w = find_worker_by_name(name);
    if (!w) return 1;     // not_found
    if (w->pid <= 0) return 3;     // already_stopped
    stop_worker(*w);
    return 0;
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

}  // namespace


void Supervisor::do_get_system_info(
        ::services::supervisor::SystemInfo& info) {
    struct utsname u{};
    if (::uname(&u) == 0) {
        info.set_hostname(u.nodename);
        std::string kernel;
        kernel.reserve(64);
        kernel.append(u.sysname).append(" ")
              .append(u.release).append(" ")
              .append(u.machine);
        info.set_kernel(kernel);
    }

    info.set_os_pretty_name(
        parse_os_release_pretty(slurp_text("/etc/os-release")));

    long cpus = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus > 0) info.set_cpu_count(static_cast<uint32_t>(cpus));

    info.set_total_ram_kb(parse_mem_total_kb(slurp_text("/proc/meminfo")));
    info.set_uptime_sec  (parse_uptime_sec  (slurp_text("/proc/uptime")));

    // Build-time facts injected via -D... when the build wires them.
    // Defaults to empty when undefined, matching .art proto3 default.
#ifdef THEIA_GIT_SHA
    info.set_theia_git_sha(THEIA_GIT_SHA);
#endif
#ifdef THEIA_BUILD_TIMESTAMP
    info.set_build_timestamp(THEIA_BUILD_TIMESTAMP);
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
    info.set_start_timestamp_ms(
        duration_cast<milliseconds>(
            started_wall.time_since_epoch()).count());
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

// ---- #386: standard GwMessageHeader-framed cast to a node ----------------
//
// The config-service receiver on a reporting FC node is a normal
// runtime TipcMux (platform/runtime/TipcMux.hh): it reads a packed
// 24-byte GwMessageHeader then dispatches `proto_len` payload bytes by
// hdr.rpc.service_id. So the supervisor must speak that wire shape, not
// the bespoke [u16 tag][payload] frame above — no second control
// format. We don't link libgw here, so mirror the packed header byte-
// for-byte (static_assert-guarded over there) and the two enum values
// we need.
#pragma pack(push, 1)
struct GwHdrWire {
    uint8_t  bus_type;        // GW_BUS_TYPE_RPC
    uint8_t  msg_type;        // GW_MSG_GEN_CAST
    uint16_t proto_len;       // payload byte count
    uint64_t timestamp_ns;    // unused for control; 0
    // union slot — we use the RPC meta (8 bytes): service_id, method_id,
    // correlation_id.
    uint16_t service_id;      // djb2_low16 of the nanopb C type name
    uint16_t method_id;       // 0
    uint32_t correlation_id;  // 0 — cast has no reply
    uint16_t tipc_seq;        // GwTipcMeta.sequence_num
    uint8_t  tipc_rsvd[2];
};
#pragma pack(pop)
static_assert(sizeof(GwHdrWire) == 24, "GwMessageHeader is 24 bytes");

constexpr uint8_t kGwBusTypeRpc  = 2u;     // GW_BUS_TYPE_RPC
constexpr uint8_t kGwMsgGenCast  = 0x20u;  // GW_MSG_GEN_CAST

// djb2_low16 — MUST match platform/runtime/RemoteCodec.hh hash_msg_type_
// so the service_id we stamp equals the one register_cast<T> computed
// for the C type name on the FC side.
uint16_t djb2_low16(const char* s) {
    uint32_t h = 5381;
    while (*s) { h = (h * 33) + static_cast<unsigned char>(*s++); }
    return static_cast<uint16_t>(h & 0xFFFFu);
}

// Send a GW_MSG_GEN_CAST frame [GwHdr][proto bytes] to a TIPC NAME.
// service_id is djb2(C type name); payload is the proto3-wire body.
// Same one-shot connect/send/close best-effort contract as above.
bool send_gw_cast_to_tipc_name(uint32_t type, uint32_t instance,
                               uint16_t service_id,
                               const std::string& payload) {
    int fd = ::socket(AF_TIPC, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    struct sockaddr_tipc addr{};
    addr.family                  = AF_TIPC;
    addr.addrtype                = TIPC_ADDR_NAME;
    addr.addr.name.name.type     = type;
    addr.addr.name.name.instance = instance;
    addr.scope                   = TIPC_NODE_SCOPE;
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }
    GwHdrWire hdr{};
    hdr.bus_type   = kGwBusTypeRpc;
    hdr.msg_type   = kGwMsgGenCast;
    hdr.proto_len  = static_cast<uint16_t>(payload.size());
    hdr.service_id = service_id;
    std::string frame(sizeof(GwHdrWire) + payload.size(), '\0');
    std::memcpy(&frame[0], &hdr, sizeof(GwHdrWire));
    if (!payload.empty()) {
        std::memcpy(&frame[sizeof(GwHdrWire)], payload.data(), payload.size());
    }
    ssize_t n = ::send(fd, frame.data(), frame.size(), 0);
    ::close(fd);
    return n == static_cast<ssize_t>(frame.size());
}

// Encode a platform_runtime.LogLevelPush { LogLevelValue level = 1 }
// as proto3 wire bytes by hand (one varint field) — avoids dragging
// nanopb into the supervisor's libprotobuf build. Field 1, wiretype 0
// (varint) → tag byte 0x08, then the level as a single-byte varint
// (values 0..4 all fit in one byte). level 0 (LL_TRACE) is the proto3
// default and would normally be omitted, but we always emit it so the
// receiver sees an explicit value.
std::string encode_log_level_push(uint32_t level) {
    std::string out;
    out.push_back('\x08');                          // field 1, varint
    out.push_back(static_cast<char>(level & 0x7f)); // 0..4 → one byte
    return out;
}

// Encode a platform_runtime.TraceControlPush { TraceKind kind = 1; bool
// enabled = 2 } as proto3 wire bytes by hand (two varint fields) — same
// no-nanopb rationale as encode_log_level_push. proto3 omits default-0
// fields, but we always emit both so the receiver sees explicit values.
// kind 0..5 fits one byte; enabled is 0/1.
std::string encode_trace_control_push(uint32_t kind, bool enabled) {
    std::string out;
    out.push_back('\x08');                          // field 1 (kind), varint
    out.push_back(static_cast<char>(kind & 0x7f));
    out.push_back('\x10');                          // field 2 (enabled), varint
    out.push_back(static_cast<char>(enabled ? 1 : 0));
    return out;
}

// service_id the FC's config_mux register_cast<platform_runtime_
// TraceControlPush> computes — djb2_low16 of that C type name. Stamped
// into the GwMessageHeader so the cast lands on the node's handle_cast.
const char* kTraceControlPushTypeName = "platform_runtime_TraceControlPush";

// Map the operator's level string → LogLevelValue ordinal (matches the
// platform.runtime LogLevelValue enum + platform::runtime::LogLevel).
uint32_t log_level_to_value(const std::string& level) {
    if (level == "trace") return 0;
    if (level == "debug") return 1;
    if (level == "info")  return 2;
    if (level == "warn" || level == "warning") return 3;
    if (level == "error" || level == "err")    return 4;
    return 2;  // default Info, same lax policy as parse_log_level
}

}  // namespace

void Supervisor::apply_trace_config(const std::string& target_node,
                                    const std::string& msg_type,
                                    bool enabled,
                                    uint32_t kind) {
    if (target_node.empty()) {
        log_warn("apply_trace_config: empty target_node — dropping");
        return;
    }
    auto& by_msg = trace_configs_[target_node];
    if (enabled) {
        by_msg[msg_type] = kind;   // value = TraceKind; presence = enabled
    } else {
        by_msg.erase(msg_type);
        // Garbage-collect empty entries so iteration in
        // push_trace_config_to_child stays cheap.
        if (by_msg.empty()) trace_configs_.erase(target_node);
    }
    std::fprintf(stderr,
        "supervisor: trace config %s for %s/%s\n",
        enabled ? "ENABLE" : "DISABLE",
        target_node.c_str(), msg_type.c_str());

    // Push immediately if the child is alive. Stale entries on dead
    // children are harmless — the next heartbeat-after-gap will fire
    // a re-push anyway.
    push_trace_config_to_child(target_node);
}

void Supervisor::push_trace_config_to_child(const std::string& child_name) {
    auto cfg_it = trace_configs_.find(child_name);
    if (cfg_it == trace_configs_.end()) return;  // nothing to push

    // Find the worker's NodeInfo to learn the NodeTraceCtl TIPC addr.
    // For Phase 1 of #361 we look up the worker by NAME and use its
    // first reporting node's tipc_{type,instance}. A future iteration
    // could route per-msg-type to different node threads, but the
    // executor.yaml today places one node per worker for FCs that
    // use trace.
    WorkerNode* w = find_worker_by_name(child_name);
    if (!w) {
        std::fprintf(stderr,
            "supervisor: trace push: no worker named '%s' yet\n",
            child_name.c_str());
        return;
    }
    if (w->nodes.empty()) {
        std::fprintf(stderr,
            "supervisor: trace push: worker '%s' has no NodeInfo "
            "(reporting=false?) — skipping push\n",
            child_name.c_str());
        return;
    }

    // Use the first reporting node's TIPC addr. Hex string → u32.
    const NodeInfo* target = nullptr;
    for (const auto& ni : w->nodes) {
        if (ni.reporting) { target = &ni; break; }
    }
    if (!target) return;

    uint32_t type, instance;
    try {
        type     = std::stoul(target->tipc_type, nullptr, 0);
        instance = std::stoul(target->tipc_instance, nullptr, 0);
    } catch (...) {
        std::fprintf(stderr,
            "supervisor: trace push: bad tipc addr for '%s'\n",
            child_name.c_str());
        return;
    }

    // One TraceControlPush GW_MSG_GEN_CAST per stored (msg_type → kind)
    // entry — the STANDARD runtime control message, decoded by the node's
    // config_mux register_cast<platform_runtime_TraceControlPush> →
    // GenServer base handle_cast → Tracer kind filter (#403). This
    // replaces the old kTagTraceApplyConfig=0x0300 [u16 tag] frame, which
    // no FC ever decoded. Entry presence = enabled; value = TraceKind.
    const uint16_t svc = djb2_low16(kTraceControlPushTypeName);
    int pushed = 0;
    for (const auto& kv : cfg_it->second) {
        uint32_t kind = kv.second;
        std::string payload = encode_trace_control_push(kind, /*enabled=*/true);
        if (send_gw_cast_to_tipc_name(type, instance, svc, payload)) {
            ++pushed;
        } else {
            std::fprintf(stderr,
                "supervisor: trace push: send to '%s' failed "
                "(type=0x%x inst=%u) — child likely not listening yet\n",
                child_name.c_str(), type, instance);
            // Don't keep iterating once a single push fails — they'll
            // all fail too. The next heartbeat-after-gap retries.
            return;
        }
    }
    std::fprintf(stderr,
        "supervisor: pushed %d trace config entries to '%s'\n",
        pushed, child_name.c_str());
}

// ---- #385: per-child log level -------------------------------------------

void Supervisor::apply_log_level(const std::string& target_node,
                                 const std::string& level) {
    if (target_node.empty()) {
        log_warn("apply_log_level: empty target_node — dropping");
        return;
    }
    log_levels_[target_node] = level;

    // Rewrite the worker's spawn env so a (re)start boots at the new
    // level (main.cc reads THEIA_LOG_LEVEL once at startup). Survives
    // restart for free — the spawn path setenvs the whole env map.
    WorkerNode* w = find_worker_by_name(target_node);
    if (w) {
        w->env["THEIA_LOG_LEVEL"] = level;
    }

    std::fprintf(stderr, "supervisor: log level for %s -> %s\n",
                 target_node.c_str(), level.c_str());

    // Push live so a running child picks it up without restart.
    push_log_level_to_child(target_node);
}

void Supervisor::push_log_level_to_child(const std::string& child_name) {
    auto it = log_levels_.find(child_name);
    if (it == log_levels_.end() || it->second.empty()) return;  // nothing to push

    WorkerNode* w = find_worker_by_name(child_name);
    if (!w) {
        std::fprintf(stderr,
            "supervisor: log push: no worker named '%s' yet\n",
            child_name.c_str());
        return;
    }
    if (w->nodes.empty()) {
        std::fprintf(stderr,
            "supervisor: log push: worker '%s' has no NodeInfo "
            "(reporting=false?) — skipping push\n",
            child_name.c_str());
        return;
    }

    // First reporting node's TIPC addr (same convention as trace).
    const NodeInfo* target = nullptr;
    for (const auto& ni : w->nodes) {
        if (ni.reporting) { target = &ni; break; }
    }
    if (!target) return;

    uint32_t type, instance;
    try {
        type     = std::stoul(target->tipc_type, nullptr, 0);
        instance = std::stoul(target->tipc_instance, nullptr, 0);
    } catch (...) {
        std::fprintf(stderr,
            "supervisor: log push: bad tipc addr for '%s'\n",
            child_name.c_str());
        return;
    }

    // Push as a standard GW_MSG_GEN_CAST of platform_runtime.LogLevelPush
    // (#386) — the SAME wire shape the FC's runtime TipcMux decodes for
    // any app message. service_id = djb2 of the nanopb C type name, which
    // is exactly what register_cast<platform_runtime_LogLevelPush> hashed
    // on the daemon side; GenServer's base handle_cast then applies it.
    static const uint16_t kLogLevelPushSvcId =
        djb2_low16("platform_runtime_LogLevelPush");
    const std::string payload =
        encode_log_level_push(log_level_to_value(it->second));
    if (send_gw_cast_to_tipc_name(type, instance,
                                  kLogLevelPushSvcId, payload)) {
        std::fprintf(stderr,
            "supervisor: pushed log level '%s' to '%s'\n",
            it->second.c_str(), child_name.c_str());
    } else {
        std::fprintf(stderr,
            "supervisor: log push: send to '%s' failed "
            "(type=0x%x inst=%u) — child not listening yet; "
            "env-applied on next restart\n",
            child_name.c_str(), type, instance);
    }
}

// T1: tell the state-manager the platform has booted. sm's statem is
// OFF --SystemBoot--> STARTING --StartupComplete--> RUNNING, so we cast
// both in sequence to sm's TIPC name (0x8001000D:0). Both are empty
// messages (zero payload); service_id = djb2 of the nanopb C type name,
// exactly what sm's register_cast<...> hashed. Same GW_MSG_GEN_CAST wire
// shape as the #386 log push. Fired once; sm_ready_sent_ guards re-send.
void Supervisor::send_sm_ready() {
    // Target the SM GATE (0x8001001D), not the statem node — the gate is
    // the FC's only TIPC-reachable surface for FSM events; it post_events
    // them into SmDaemon in-process (services/system/sm: SmGate node).
    constexpr uint32_t kSmTipcType     = 0x8001001Du;
    constexpr uint32_t kSmTipcInstance = 0u;
    static const uint16_t kSystemBootSvcId =
        djb2_low16("services_services_sm_SystemBoot");
    static const uint16_t kStartupCompleteSvcId =
        djb2_low16("services_services_sm_StartupComplete");

    const std::string empty;  // both messages have no fields
    const bool b1 = send_gw_cast_to_tipc_name(
        kSmTipcType, kSmTipcInstance, kSystemBootSvcId, empty);
    const bool b2 = send_gw_cast_to_tipc_name(
        kSmTipcType, kSmTipcInstance, kStartupCompleteSvcId, empty);
    if (b1 && b2) {
        log_info("sm startup handshake sent (SystemBoot + StartupComplete)");
    } else {
        std::fprintf(stderr,
            "supervisor: sm startup handshake send failed "
            "(SystemBoot=%d StartupComplete=%d) — sm not listening yet\n",
            b1, b2);
    }
}

}  // namespace supervisor

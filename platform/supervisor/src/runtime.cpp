// Supervisor runtime — fork/exec children, observe exits, apply OTP-style
// restart strategy. Mirrors supervisor/runtime.py.

#include "supervisor/runtime.h"

// Generated from services/supervisor/system/package.art by
// `artheia gen-proto` and compiled by protoc via the CMake build.
#include "ChildState.pb.h"
#include "HealthBeacon.pb.h"
#include "SupervisionEvent.pb.h"
#include "TreeSnapshot.pb.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

Supervisor::Supervisor(std::unique_ptr<Node> root, std::string root_dir)
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
// Inbound tags (filled by phase 3 / phase 4):
constexpr uint16_t kTagControlRequest = 0x0100;
constexpr uint16_t kTagControlReply   = 0x0101;
constexpr uint16_t kTagHeartbeat      = 0x0200;
constexpr uint16_t kTagSendTimeout    = 0x0201;
}  // namespace

// Inbound TIPC dispatch. Phase 3 fills the ControlRequest branch;
// phase 4 fills the heartbeat + send-timeout branches. For now the
// body just logs unknown frames so we can see them arrive.
void Supervisor::on_inbound_frame(int /*client_fd*/, uint16_t tag,
                                  const std::string& payload) {
    switch (tag) {
        case kTagControlRequest:
            // TODO(phase 3): decode ControlRequest, dispatch by op.
            std::fprintf(stderr, "supervisor: ControlRequest (%zu bytes) — "
                         "phase 3 placeholder\n", payload.size());
            break;
        case kTagHeartbeat:
            // TODO(phase 4): decode HeartbeatReport, refresh watchdog.
            break;
        case kTagSendTimeout:
            // TODO(phase 4): decode SendTimeoutReport, emit_event(kind=send_timeout).
            break;
        default:
            std::fprintf(stderr, "supervisor: unknown inbound tag 0x%04x "
                         "(%zu bytes)\n", tag, payload.size());
            break;
    }
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

bool read_proc_status(pid_t pid, uint64_t* rss_kb, uint64_t* vsz_kb) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/status", static_cast<int>(pid));
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char line[256];
    bool got_rss = false, got_vsz = false;
    while (std::fgets(line, sizeof(line), f)) {
        if (!got_rss && std::strncmp(line, "VmRSS:", 6) == 0) {
            unsigned long v = 0;
            if (std::sscanf(line + 6, "%lu", &v) == 1) { *rss_kb = v; got_rss = true; }
        } else if (!got_vsz && std::strncmp(line, "VmSize:", 7) == 0) {
            unsigned long v = 0;
            if (std::sscanf(line + 7, "%lu", &v) == 1) { *vsz_kb = v; got_vsz = true; }
        }
        if (got_rss && got_vsz) break;
    }
    std::fclose(f);
    return got_rss && got_vsz;
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
        uint64_t rss = 0, vsz = 0;
        if (read_proc_status(w->pid, &rss, &vsz)) {
            s.rss_kb = rss;
            s.vsz_kb = vsz;
        }
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
                    auto* row = snap.add_children();
                    row->set_name(c->worker.name);
                    row->set_parent_name(sup.name);
                    row->set_kind(0);
                    row->set_pid(c->worker.pid);
                    row->set_state(state);
                    row->set_restart_count(0);
                    row->set_start_cmd(cmd);
                    // Resource samples — taken in sample_procs() above.
                    if (c->worker.pid > 0) {
                        auto it = sample_.find(c->worker.pid);
                        if (it != sample_.end()) {
                            row->set_cpu_pct(it->second.cpu_pct);
                            row->set_rss_kb (it->second.rss_kb);
                            row->set_vsz_kb (it->second.vsz_kb);
                            row->set_threads(it->second.threads);
                        }
                    }
                } else {
                    walk(c->sup, sup.name);
                }
            }
        };
    walk(*root_, "");

    publisher_.publish(kTagSnapshot, snap.SerializeAsString());
}

}  // namespace supervisor

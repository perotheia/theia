// theia::runtime::GenRunnable<Derived>
//
// The third node base, beside GenServer<Derived,State> (reactive, mailbox)
// and GenStateM<Derived,…> (FSM). GenRunnable is for a FREE RUNNABLE: a
// worker that owns a thread and does its own thing rather than reacting to a
// message mailbox. The motivating case is a gRPC proxy thread (services/com):
// it is not a gen_server, and before GenRunnable the only home for it was a
// hand-written main.cc — exactly the runtime-generality leak we close here.
//
// OTP shape: gen_server (reactive) / gen_statem (FSM) / gen_runnable (plain
// worker, ~ proc_lib:spawn_link). All three are owned uniformly by the
// thread executor: the generated main calls start() / stop() on each,
// regardless of kind.
//
// ---- the do_* contract (what Derived overrides) -------------------------
//
//   void do_start()   — one-time setup on the worker thread before the loop
//                        (e.g. build + start the gRPC server). Optional.
//   void do_loop()    — the body. Called once on the worker thread. It runs
//                        until it RETURNS — the runnable is then done. The
//                        base does not re-invoke it.
//   void do_stop()    — release resources + signal the loop to end. Runs on
//                        the caller's thread from stop(); the worker is joined
//                        after. Optional.
//
//   bool stop_requested() const — the single stop signal; stop() sets it.
//
// ---- how do_loop ends: the user's choice, not the base's burden ---------
//
// The base owns one atomic, exposed as stop_requested(). stop() flips it.
// How do_loop() observes it is the runnable author's call:
//
//   cooperative (the common case):
//       void do_loop() { while (!stop_requested()) { work(); } }
//
//   periodic with heartbeat (reporting nodes):
//       void do_loop() {
//           while (!stop_requested()) { server_->Wait(deadline_ms); beat(); }
//       }
//       — a Wait()-with-timeout loop both serves requests AND beats the
//       watchdog itself, so no separate heartbeat thread is needed. This is
//       why the base does NOT spawn one.
//
//   genuinely blocking (no timeout, e.g. bare server->Wait()):
//       void do_loop() { server_->Wait(); }          // returns on Shutdown
//       void do_stop() { server_->Shutdown(); }      // wakes the Wait()
//       — fine for a reporting=false runnable; if it must beat, use the
//       periodic form above instead.
//
// So: heartbeat is the user's choice via reporting + how they write do_loop
// (reporting=false → no beat needed; a Wait(timeout) loop → beat per tick).
// kReporting is still read (below) so the generated main / supervisor know
// whether to watchdog this node — the base just doesn't add a thread for it.

#pragma once

#include "NodeRef.hh"   // theia::runtime::cast(self, msg, TipcAddr) + TipcAddr

#include <atomic>
#include <string>
#include <thread>

namespace theia {
namespace runtime {

template <typename Derived>
class GenRunnable {
public:
    GenRunnable() { mark_reporting_(); }
    virtual ~GenRunnable() { force_stop_(); }

    GenRunnable(const GenRunnable&)            = delete;
    GenRunnable& operator=(const GenRunnable&) = delete;

    // Uniform with GenServer::start(): spawn the worker thread, which runs
    // do_start() then do_loop().
    void start() {
        if (running_.exchange(true)) return;
        worker_ = std::thread([this] {
            static_cast<Derived*>(this)->do_start();
            static_cast<Derived*>(this)->do_loop();
        });
    }

    // Uniform with GenServer::stop(): flip the stop atomic, let do_stop()
    // release/wake, join the worker. Idempotent.
    void stop(const std::string& reason = "normal") {
        if (!running_.exchange(false)) return;  // running_ false ⇒ stop_requested()
        terminate_reason_ = reason;
        static_cast<Derived*>(this)->do_stop();
        if (worker_.joinable()) worker_.join();
    }

    // The single stop signal. do_loop() polls this to exit cooperatively.
    bool stop_requested() const { return !running_.load(); }

    // Whether this runnable is alive (started, not yet stopped). Inverse of
    // stop_requested(); offered for readability inside do_loop().
    bool running() const { return running_.load(); }

    // Cast a typed message to a remote node addressed at runtime (the peer's
    // TipcAddr resolved at run time, e.g. a supervised child's address read from
    // executor.json — NOT a compile-time netgraph constant). Thin override of
    // the runtime's addressed cast: builds a one-shot RemoteRef from `addr`,
    // encodes via RemoteCodec<Msg> (service_id + wire bytes match the peer's
    // register_cast<Msg>), and trace-tags the Send with THIS node as src. Lets a
    // runnable forward control to children via `this->cast(msg, addr)` instead
    // of the free function. Best-effort fire-and-forget. Public — it's a node→
    // peer API the FC shell uses (e.g. the supervisor's config push).
    template <typename Msg>
    void cast(const Msg& msg, ::theia::runtime::TipcAddr addr) {
        ::theia::runtime::cast(*static_cast<Derived*>(this), msg, addr);
    }

    // ---- defaults Derived may override --------------------------------
    // do_start / do_stop default to no-ops; do_loop has no default (a
    // runnable with no body is a programming error — Derived must define it).
    void do_start() {}
    void do_stop() {}

protected:
    const std::string& terminate_reason() const { return terminate_reason_; }

    // AUTOSAR Reporting flag (from the .art `reporting`, via Derived's
    // kReporting, default false). The base does NOT act on it — heartbeat is
    // the user's choice in do_loop (see header note). Exposed so the
    // generated main / supervisor watchdog know whether this node beats.
    bool is_reporting() const { return reporting_; }

private:
    void force_stop_() {
        if (running_.load()) stop("forced");
    }

    // SFINAE: kReporting is optional on Derived (defaults false), same
    // detector trick GenServer uses for its reporting gate.
    template <typename D>
    static auto reporting_of_(int) -> decltype(D::kReporting, bool{}) {
        return D::kReporting;
    }
    template <typename D>
    static bool reporting_of_(...) { return false; }
    void mark_reporting_() { reporting_ = reporting_of_<Derived>(0); }

    std::atomic<bool>  running_{false};
    bool               reporting_{false};
    std::thread        worker_;
    std::string        terminate_reason_{"normal"};
};

}  // namespace runtime
}  // namespace theia

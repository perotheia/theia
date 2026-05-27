// demo::runtime::GenRunnable<Derived>
//
// The third node base, beside GenServer<Derived,State> (reactive, mailbox)
// and GenStateM<Derived,…> (FSM). GenRunnable is for a FREE RUNNABLE: a
// worker that owns a thread and does its own thing — typically blocking —
// rather than reacting to a message mailbox. The motivating case is a gRPC
// proxy thread (services/com): it blocks in Server::Wait(), it is not a
// gen_server, and before GenRunnable the only home for it was a hand-written
// main.cc — which is exactly the runtime-generality leak we close here.
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
//   void do_loop()    — the body. MAY BLOCK for the runnable's whole life
//                        (e.g. server->Wait()). Called once. If it returns,
//                        the runnable is done. The base does NOT re-invoke it
//                        in a tight loop — a runnable that wants a poll loop
//                        writes its own `while (!stop_requested()) { … }`.
//   void do_stop()    — MUST unblock do_loop() (e.g. server->Shutdown()) and
//                        release resources. Runs on the caller's thread from
//                        stop(); the worker thread is joined after.
//
//   bool stop_requested() const — poll inside a do_loop() that loops, so the
//                        loop can exit cooperatively on stop().
//
// The "do_stop unblocks do_loop" contract is the clean half of the design:
// a blocking do_loop() (grpc Wait()) is fine because do_stop() (grpc
// Shutdown()) wakes it; a polling do_loop() checks stop_requested().
//
// ---- reporting=true → watchdog heartbeat --------------------------------
//
// A blocking do_loop() can't beat a heartbeat from inside itself, so the
// base owns a SEPARATE cadence timer thread (only when kReporting is true)
// that calls do_heartbeat() every kHeartbeatPeriodMs. That keeps a runnable
// a first-class supervised citizen — same as a reporting gen_server — no
// matter what do_loop() blocks on. do_heartbeat()'s default is a no-op; the
// nanopb heartbeat sender wires into it when that lands (it is deliberately
// NOT coupled to the libprotobuf HeartbeatPublisher here).

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace demo {
namespace runtime {

template <typename Derived>
class GenRunnable {
public:
    GenRunnable() { mark_reporting_(); }
    virtual ~GenRunnable() { force_stop_(); }

    GenRunnable(const GenRunnable&)            = delete;
    GenRunnable& operator=(const GenRunnable&) = delete;

    // Uniform with GenServer::start(): spawn the worker thread (runs
    // do_start then do_loop). If kReporting, also spawn the heartbeat timer.
    void start() {
        if (running_.exchange(true)) return;
        worker_ = std::thread([this] {
            static_cast<Derived*>(this)->do_start();
            static_cast<Derived*>(this)->do_loop();
        });
        if (reporting_) {
            hb_ = std::thread([this] { heartbeat_loop_(); });
        }
    }

    // Uniform with GenServer::stop(): signal stop, unblock do_loop via
    // do_stop(), join. Idempotent.
    void stop(const std::string& reason = "normal") {
        if (!running_.exchange(false)) return;
        terminate_reason_ = reason;
        {
            std::lock_guard<std::mutex> lk(hb_mu_);
            hb_cv_.notify_all();           // wake the heartbeat timer to exit
        }
        static_cast<Derived*>(this)->do_stop();  // MUST unblock do_loop()
        if (worker_.joinable()) worker_.join();
        if (hb_.joinable())     hb_.join();
    }

    // Poll inside a cooperative do_loop().
    bool stop_requested() const { return !running_.load(); }

    // ---- defaults Derived may override --------------------------------
    // do_start / do_stop default to no-ops; do_loop has no default (a
    // runnable with no body is a programming error — Derived must define it).
    void do_start() {}
    void do_stop() {}

    // reporting=true heartbeat tick. Default no-op; the nanopb heartbeat
    // sender overrides this. Runs on the base's heartbeat timer thread,
    // independent of do_loop().
    void do_heartbeat() {}

    // Heartbeat cadence (ms). Derived may shadow with its own constant.
    static constexpr uint32_t kHeartbeatPeriodMs = 1000;

protected:
    const std::string& terminate_reason() const { return terminate_reason_; }

private:
    void heartbeat_loop_() {
        std::unique_lock<std::mutex> lk(hb_mu_);
        while (running_.load()) {
            hb_cv_.wait_for(lk,
                std::chrono::milliseconds(Derived::kHeartbeatPeriodMs),
                [this] { return !running_.load(); });
            if (!running_.load()) break;
            lk.unlock();
            static_cast<Derived*>(this)->do_heartbeat();
            lk.lock();
        }
    }

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

    std::atomic<bool>        running_{false};
    bool                     reporting_{false};
    std::thread              worker_;
    std::thread              hb_;
    std::mutex               hb_mu_;
    std::condition_variable  hb_cv_;
    std::string              terminate_reason_{"normal"};
};

}  // namespace runtime
}  // namespace demo

// Minimal Timer impl: spawns a detached thread that sleeps `period`
// and fires the callback, until cancel(). Adequate for low-frequency
// host work (50ms-1s ticks). The gateway's own reactor (libgw's
// gw_tipc_server) is the right home for higher-rate I/O timers; the
// generated app's Inputs DI bundle gets one of these instead.

#include "Timer.hh"

#include <atomic>
#include <chrono>
#include <thread>

namespace platform {
namespace runtime {

namespace {

class ThreadTimer : public Timer {
 public:
  ~ThreadTimer() noexcept override { cancel(); }

  void start(std::chrono::milliseconds period, Callback cb) noexcept override {
    cancel();
    if (!cb) return;
    running_.store(true);
    std::thread([this, period, cb = std::move(cb)]() {
      while (running_.load()) {
        std::this_thread::sleep_for(period);
        if (!running_.load()) break;
        cb();
      }
    }).detach();
  }

  bool cancel() noexcept override {
    bool was_running = running_.exchange(false);
    // Detached thread observes the flag and exits on its own; we don't
    // join, so cancel() doesn't block. Brief overlap with one more
    // callback invocation is possible; users should treat callbacks as
    // best-effort and idempotent.
    return was_running;
  }

 private:
  std::atomic<bool> running_{false};
};

class ThreadTimerFactory : public TimerFactoryInterface {
 public:
  std::shared_ptr<Timer> CreateTimer() noexcept override {
    return std::make_shared<ThreadTimer>();
  }
};

}  // namespace

// Factory hook used by RuntimeContext construction.
std::shared_ptr<TimerFactoryInterface> MakeThreadTimerFactory() noexcept {
  return std::make_shared<ThreadTimerFactory>();
}

}  // namespace runtime
}  // namespace platform

// theia::runtime::Timer
//
// Periodic callback scheduler. The app asks the runtime for a Timer,
// registers a callback + period, then forgets about it; the runtime
// fires the callback on its own scheduling thread. Cancel returns
// false if the timer wasn't running.

#pragma once

#include <chrono>
#include <functional>
#include <memory>

namespace theia {
namespace runtime {

class Timer {
 public:
  using Callback = std::function<void()>;

  virtual ~Timer() = default;

  // Start firing `cb` every `period`. Returns immediately. If the
  // timer is already started, replaces the callback + period.
  virtual void start(std::chrono::milliseconds period, Callback cb) noexcept = 0;

  // Stop firing. Returns true if a previously-running timer was stopped.
  virtual bool cancel() noexcept = 0;
};

// Factory hook: the runtime's TimerFactoryInterface produces these.
// Generated apps receive a Timer via the Inputs DI bundle.
class TimerFactoryInterface {
 public:
  virtual ~TimerFactoryInterface() = default;
  virtual std::shared_ptr<Timer> CreateTimer() noexcept = 0;
};

// Default factory: produces detached-thread Timers (best-effort, suitable
// for 50ms-1s ticks). The gateway's reactor would be a better backend for
// high-frequency I/O timers; for now this lets the generated apps run.
std::shared_ptr<TimerFactoryInterface> MakeThreadTimerFactory() noexcept;

}  // namespace runtime
}  // namespace theia

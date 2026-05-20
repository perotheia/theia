// platform::runtime::Clock
//
// Time source the app uses to make timing decisions. Wrapped in an
// interface so tests can substitute a MockClock that advances on demand.

#pragma once

#include <chrono>
#include <memory>

namespace platform {
namespace runtime {

class Clock {
 public:
  using time_point = std::chrono::steady_clock::time_point;
  using duration   = std::chrono::steady_clock::duration;

  virtual ~Clock() = default;

  // Monotonic wall time. Use this for elapsed-time math; do not use
  // for absolute calendar timestamps (the gateway header carries those).
  virtual time_point now() const noexcept = 0;
};

// Default: forwards to std::chrono::steady_clock.
class SteadyClock : public Clock {
 public:
  time_point now() const noexcept override;
};

std::shared_ptr<Clock> MakeSteadyClock() noexcept;

}  // namespace runtime
}  // namespace platform

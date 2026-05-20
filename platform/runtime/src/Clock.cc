#include "Clock.hh"

namespace platform {
namespace runtime {

Clock::time_point SteadyClock::now() const noexcept {
    return std::chrono::steady_clock::now();
}

std::shared_ptr<Clock> MakeSteadyClock() noexcept {
    return std::make_shared<SteadyClock>();
}

}  // namespace runtime
}  // namespace platform

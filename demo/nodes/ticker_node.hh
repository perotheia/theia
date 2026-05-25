// TickerNode — self-scheduling periodic node. Exercises the
// schedule/handle/reschedule pattern via TimerService::send_after.
//
// In the .art TickerNode has no ports, so its only dep is the timer
// service. Each `loop` info reschedules itself. After max_ticks
// iterations the loop stops by simply not rescheduling.

#pragma once

#include "GenServer.hh"
#include "Logger.hh"
#include "TimerService.hh"

#include <cstdint>
#include <memory>

namespace demo {

struct TickerState {
    uint32_t ticks_fired = 0;
};

struct TickerNodeInputs {
    std::shared_ptr<platform::runtime::Logger> logger;
    runtime::TimerService& timers;
    uint32_t max_ticks = 30;
};

class TickerNode
    : public runtime::GenServer<TickerNode, TickerState> {
public:
    static constexpr const char* kNodeName = "TickerNode";

    explicit TickerNode(const TickerNodeInputs& in);

    void kick_off();

    void handle_info(const char* info, TickerState& s);

private:
    std::shared_ptr<platform::runtime::Logger> logger_;
    runtime::TimerService& timers_;
    uint32_t               max_ticks_;
};

}  // namespace demo

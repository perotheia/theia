// User handler bodies for TickerNode.
//
// Self-driving node: init() posts "loop"; each "loop" self-schedules the
// next via send_after, up to a fixed tick count. Migrated from the
// retired demo/nodes/ticker_node.{hh,cc} onto the gen-app --kind fc shape
// — timers via process_timers() (the .art `requires_timers` flag makes
// main publish it), logger via this->log() ([#ticker] tag).

#include "lib/TickerNode.hh"

#include "TimerService.hh"   // post_info / send_after / process_timers

#include <cstring>
#include <string>

namespace demo {

namespace {
constexpr int kMaxTicks = 10;
}

// OTP init/1: kick off the self-loop (was kick_off()).
void TickerNode::init(TickerNodeState& /*s*/) {
    theia::runtime::post_info(*this, "loop");
}

void TickerNode::handle_info(const char* info, TickerNodeState& s) {
    if (std::strcmp(info, "loop") != 0) return;

    if (s.ticks_fired >= kMaxTicks) {
        this->log().info("done after " + std::to_string(s.ticks_fired) +
                         " ticks");
        return;
    }
    ++s.ticks_fired;
    theia::runtime::send_after(theia::runtime::process_timers(), 100, *this,
                              "loop");
}

}  // namespace demo

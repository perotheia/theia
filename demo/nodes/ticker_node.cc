#include "ticker_node.hh"

#include <cstring>

namespace demo {

TickerNode::TickerNode(const TickerNodeInputs& in)
    : logger_(in.logger), timers_(in.timers), max_ticks_(in.max_ticks) {}

void TickerNode::kick_off() {
    runtime::post_info(*this, "loop");
}

void TickerNode::handle_info(const char* info, TickerState& s) {
    if (std::strcmp(info, "loop") != 0) return;

    if (s.ticks_fired >= max_ticks_) {
        logger_->info("[ticker] done after " +
                      std::to_string(s.ticks_fired) + " ticks");
        return;
    }
    ++s.ticks_fired;
    runtime::send_after(timers_, 100, *this, "loop");
}

}  // namespace demo

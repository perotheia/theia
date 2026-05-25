#include "counter_node.hh"

#include <cstring>

namespace demo {

CounterNode::CounterNode(const CounterNodeInputs& in)
    : logger_(in.logger) {}

services_demo_GetReply
CounterNode::handle_call(const services_demo_Get& /*req*/,
                          CounterState& s) {
    logger_->info("[counter] handle_call(Get) → " + std::to_string(s.counter));
    services_demo_GetReply reply{};
    reply.value = s.counter;
    return reply;
}

void CounterNode::handle_cast(const services_demo_Inc& msg,
                               CounterState& s) {
    s.counter += msg.n;
    // No log per cast — DriverNode does 10 of these and the noise is
    // not useful. The tick handler prints the cumulative value.
}

void CounterNode::handle_info(const char* info, CounterState& s) {
    if (std::strcmp(info, "tick") == 0) {
        logger_->info("[counter] tick — counter=" + std::to_string(s.counter));
    } else {
        logger_->info(std::string("[counter] handle_info(other) — ") + info);
    }
}

void CounterNode::terminate(const char* reason, CounterState& s) noexcept {
    s.terminated_with = reason;
    logger_->info(std::string("[counter] terminate(") + reason +
                  ") — final counter=" + std::to_string(s.counter));
}

}  // namespace demo

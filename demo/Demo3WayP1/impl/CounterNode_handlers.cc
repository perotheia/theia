// User handler bodies for CounterNode.
//
// Holds an integer; serves Get (call) and Inc (cast). Migrated from the
// retired demo/nodes/counter_node.{hh,cc} onto the gen-app --kind fc
// shape (platform/runtime GenServer; logger via process_logger()).

#include "lib/CounterNode.hh"

#include "Logger.hh"   // process_logger()

#include <cstring>
#include <string>

namespace demo {

// CounterNode is passive — no startup work.
void CounterNode::init(CounterNodeState& /*s*/) {
}

// handle_info: a periodic "tick" logs the cumulative counter.
void CounterNode::handle_info(const char* info, CounterNodeState& s) {
    if (std::strcmp(info, "tick") == 0) {
        ::theia::runtime::process_logger().info(
            "[counter] tick — counter=" + std::to_string(s.counter));
    }
}

// Inc — cast, no reply. Accumulate.
void CounterNode::handle_cast(const Inc& msg, CounterNodeState& s) {
    s.counter += msg.n;
}

// Get — call, replies with the current counter.
GetReply CounterNode::handle_call(const Get& /*req*/, CounterNodeState& s) {
    ::theia::runtime::process_logger().info(
        "[counter] handle_call(Get) -> " + std::to_string(s.counter));
    GetReply reply{};
    reply.value = s.counter;
    return reply;
}

}  // namespace demo

// User handler bodies for CounterNode.
//
// Holds an integer; serves Get (call) and Inc (cast). Migrated from the
// retired demo/nodes/counter_node.{hh,cc} onto the gen-fc --kind fc
// shape (platform/runtime GenServer; logger via this->log() — the node's own
// [#counter]-tagged logger from the NodeLogger mixin).

#include "lib/CounterNode.hh"

#include <cstring>
#include <string>

namespace system_apps {

// CounterNode is passive — no startup work.
void CounterNode::init(CounterNodeState& /*s*/) {
}

// handle_info: a periodic "tick" logs the cumulative counter.
void CounterNode::handle_info(const char* info, CounterNodeState& s) {
    if (std::strcmp(info, "tick") == 0) {
        this->log().debug("tick — counter=" + std::to_string(s.counter));
    }
}

// Inc — cast, no reply. Accumulate.
void CounterNode::handle_cast(const Inc& msg, CounterNodeState& s) {
    s.counter += msg.n;
}

// Get — call, replies with the current counter.
GetReply CounterNode::handle_call(const Get& /*req*/, CounterNodeState& s) {
    this->log().debug("handle_call(Get) -> " + std::to_string(s.counter));
    GetReply reply{};
    reply.value = s.counter;
    return reply;
}


// ---- config update — services/per casts ConfigUpdated when this node's
//      etcd-backed config changes; the GenServer base decoded + logged. This
//      demo node reads its config at boot (get_config()), so the live hook is
//      an empty default (the declaration shadows the base no-op).
void CounterNode::on_config_update(
        const platform_runtime_ConfigUpdated& /*cfg*/,
        CounterNodeState& /*s*/) {
}

}  // namespace system_apps

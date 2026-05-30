// User handler bodies for DriverNode.
//
// Drives the demo: on init, fire 10 casts of Inc{5} at CounterNode, then
// a synchronous call(Get) and check the reply is 50. Migrated from the
// retired demo/nodes/driver_node.{hh,tcc} onto the gen-app --kind fc
// shape — cross-node messaging via the generated netgraph (cast by
// TipcAddr, call by RemoteRef alias); logger via process_logger().

#include "lib/DriverNode.hh"
#include "lib/DriverNode_netgraph.hh"   // netgraph::counternode + CounterNodeRef

#include "Logger.hh"

#include <cstring>
#include <string>

namespace demo {

namespace {
// Caller-side correlation tag handed to call<>(); echoed back in the
// CallResult so a handler can tell which request a reply belongs to.
struct DriverAct {
    uint32_t request_id = 0;
};
}

// OTP init/1: bootstrap the run sequence (was kick_off()).
void DriverNode::init(DriverNodeState& /*s*/) {
    theia::runtime::post_info(*this, "run");
}

void DriverNode::handle_info(const char* info, DriverNodeState& s) {
    if (std::strcmp(info, "run") != 0) return;

    auto& log = ::theia::runtime::process_logger();
    log.info("[driver] starting: 10x cast(Inc{5}), then call(Get)");

    // Fan out 10 increments of 5 to CounterNode (fire-and-forget, by
    // netgraph TipcAddr).
    for (int i = 0; i < 10; ++i) {
        Inc msg{};
        msg.n = 5;
        theia::runtime::cast(*this, msg, demo::netgraph::counternode);
    }
    s.expected_value = 50;

    // Synchronous call(Get): connect the RemoteRef to CounterNode (its
    // address baked into the netgraph alias), then call and check.
    demo::netgraph::CounterNodeRef counter;
    if (!counter.connect()) {
        log.error("[driver] could not connect to CounterNode");
        return;
    }
    DriverAct act{/*request_id=*/1};
    auto r = theia::runtime::call<GetReply>(counter, Get{}, act,
                                           /*timeout_ms=*/2000);
    switch (r.tag) {
        case theia::runtime::CallTag::Reply:
            s.last_value = r.reply.value;
            ++s.replies_ok;
            log.info("[driver] handle_call_result(req_id=" +
                     std::to_string(r.act.request_id) +
                     ") value=" + std::to_string(r.reply.value) +
                     " expected=" + std::to_string(s.expected_value));
            break;
        case theia::runtime::CallTag::Timeout:
            log.error("[driver] call(Get) timed out");
            break;
        case theia::runtime::CallTag::Error:
            log.error(std::string("[driver] call(Get) error: ") + r.error);
            break;
    }
}

}  // namespace demo

// User handler bodies for IncrementerNode.
//
// Every 300ms casts Inc{2} at CounterNode (cross-PROCESS — Counter is in
// P1). Migrated from the retired demo/nodes/incrementer_node.{hh,tcc}
// onto the gen-app --kind fc shape — outbound cast by the generated
// netgraph TipcAddr; timers via process_timers(), logger via this->log()
// ([#incrementer] tag).

#include "lib/IncrementerNode.hh"
#include "lib/IncrementerNode_netgraph.hh"   // netgraph::counternode

#include "TimerService.hh"

#include <cstring>
#include <string>

namespace demo {

void IncrementerNode::init(IncrementerNodeState& /*s*/) {
    theia::runtime::post_info(*this, "tick");
}

void IncrementerNode::handle_info(const char* info, IncrementerNodeState& s) {
    if (std::strcmp(info, "tick") != 0) return;

    Inc msg{};
    msg.n = 2;
    theia::runtime::cast(*this, msg, demo::netgraph::counternode);
    ++s.casts_sent;

    if ((s.casts_sent % 10) == 0) {
        this->log().info("casts_sent=" + std::to_string(s.casts_sent));
    }
    theia::runtime::send_after(theia::runtime::process_timers(), 300, *this,
                              "tick");
}

}  // namespace demo

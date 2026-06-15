// User handler bodies for ObserverNode.
//
// Polls CounterNode.Get every 200ms (cross-PROCESS call — Observer is its
// own process; Counter lives in P1). Migrated from the retired
// demo/nodes/observer_node.{hh,tcc} onto the gen-app --kind fc shape —
// the call goes through the generated netgraph RemoteRef (peer address
// from the cluster connect observer.counter_call -> counter.srv); timers
// via process_timers(), logger via this->log() ([#observer] tag).

#include "lib/ObserverNode.hh"
#include "lib/ObserverNode_netgraph.hh"   // netgraph::CounterNodeRef

#include "TimerService.hh"

#include <cstring>
#include <string>

namespace demo {

namespace {
struct ObserverAct {
    uint32_t request_id = 0;
};
}

// OTP init/1: start the poll loop (was kick_off()).
void ObserverNode::init(ObserverNodeState& /*s*/) {
    theia::runtime::post_info(*this, "poll");
}

void ObserverNode::handle_info(const char* info, ObserverNodeState& s) {
    if (std::strcmp(info, "poll") != 0) return;

    // One RemoteRef for the node's lifetime would be ideal; the demo
    // reconnects per poll (low frequency) to keep the handler self-
    // contained. connect() is idempotent-cheap for a live peer.
    demo::netgraph::CounterNodeRef counter;
    if (counter.connect(/*timeout_ms=*/500)) {
        ObserverAct act{static_cast<uint32_t>(++s.polls_issued)};
        auto r = theia::runtime::call<GetReply>(counter, Get{}, act,
                                               /*timeout_ms=*/500);
        switch (r.tag) {
            case theia::runtime::CallTag::Reply:
                s.last_value = r.reply.value;
                ++s.replies_ok;
                this->log().debug(
                    "poll #" + std::to_string(r.act.request_id) +
                    " value=" + std::to_string(r.reply.value));
                break;
            case theia::runtime::CallTag::Timeout:
                this->log().error("timeout req_id=" +
                                  std::to_string(r.act.request_id));
                break;
            case theia::runtime::CallTag::Error:
                this->log().error(std::string("error: ") + r.error);
                break;
        }
    }
    theia::runtime::send_after(theia::runtime::process_timers(), 200, *this,
                              "poll");
}


// ---- config update — services/per casts ConfigUpdated when this node's
//      etcd-backed config changes; the GenServer base decoded + logged. This
//      demo node reads its config at boot (get_config()), so the live hook is
//      an empty default (the declaration shadows the base no-op).
void ObserverNode::on_config_update(
        const platform_runtime_ConfigUpdated& /*cfg*/,
        ObserverNodeState& /*s*/) {
}

}  // namespace demo

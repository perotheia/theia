// SupervisorControlNode — the supervisor's control surface as a
// platform/runtime gen_server node on TipcMux.
//
// The supervisor stays a bespoke fork/exec orchestrator (the select() loop in
// Supervisor::run()). This node is its I/O FRONT-END: a GenServer bound on
// TipcMux at a DISTINCT control address (0x80020003/0 — NOT the publisher's
// 0x80020001), exposing the control surface over the STANDARD Theia transport
// (GwMessageHeader + nanopb, the same wire every FC speaks). com drives it with
// RemoteRef. See docs/com-supervisor-transport.md §5.
//
// register_call<ControlRequest, ControlReply> dispatches an inbound CALL to
// handle_call(), which thunks straight into the orchestrator via
// Supervisor::dispatch_control_nanopb(). The TipcMux epoll thread and the
// main select() loop share the Supervisor via the back-pointer; the
// orchestrator guards its own mutable state (see dispatch_control_nanopb).
//
// This is the OTP-faithful split: gen_server front-end, orchestration as the
// business logic it calls.

#pragma once

#include "GenServer.hh"
#include "ControlRequest.pb.h"   // nanopb (supervisor_nanopb include root)
#include "ControlReply.pb.h"

namespace supervisor {

class Supervisor;  // fwd — control_node.cpp + runtime.cpp include the full def

struct SupervisorControlState {
    Supervisor* sup = nullptr;   // back-pointer to the orchestrator
};

class SupervisorControlNode
    : public demo::runtime::GenServer<SupervisorControlNode,
                                      SupervisorControlState> {
public:
    static constexpr const char* kNodeName = "supervisor_ctl";
    // DISTINCT control TIPC type for the standard-transport control surface.
    // NOT 0x80020001 — that address stays bound by the legacy TipcPublisher
    // for the event/health/snapshot firehose (and the two-frame GetTree read).
    // Binding the control node on a separate type (0x80020003/0) avoids a TIPC
    // name-collision where an inbound control CALL could route to the
    // publisher's accepting socket instead of this gen_server. com reaches it
    // via RemoteRef<SupervisorControlNode, 0x80020003, 0>.
    static constexpr uint32_t kTipcType     = 0x80020003u;
    static constexpr uint32_t kTipcInstance = 0u;
    // Not a reporting FC node — no heartbeat / watchdog / trace-push for the
    // supervisor's own control node.
    static constexpr bool kReporting = false;

    explicit SupervisorControlNode(Supervisor* sup) {
        state().sup = sup;
    }

    // Inbound control CALL: ControlRequest -> ControlReply, both nanopb.
    // Defined in control_node.cpp (needs the full Supervisor definition).
    services_supervisor_ControlReply handle_call(
        const services_supervisor_ControlRequest& req,
        SupervisorControlState& s);
};

}  // namespace supervisor

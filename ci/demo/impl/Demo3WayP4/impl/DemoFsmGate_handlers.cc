// User handler bodies for DemoFsmGate — the test FSM's external-message
// front (modeled on services/sm's SmGate).
//
// HAND-OWNED (gen-fc emits this once, then skips it without --force).
// Each event the gate receives over TIPC is forwarded into the DemoFsm
// state machine IN-PROCESS via post_event(). The statem node itself takes
// no external messages — the gate is the only TIPC-reachable surface for
// FSM events, so the wire never drives the FSM directly.
//
// Cross-node reach: both DemoFsm and DemoFsmGate run as threads in this one
// p4 process. DemoFsm::on_enter publishes a LocalRef<DemoFsm> into the
// shared singleton on its first state entry (during start_statem), before
// any gate event can matter; the gate reads it here.

#include "lib/DemoFsmGate.hh"
#include "lib/DemoFsm.hh"

#include "GenStateM.hh"   // theia::runtime::post_event
#include "NodeRef.hh"     // theia::runtime::LocalRef

#include <cstdio>

namespace system_apps {

// The statem peer the gate forwards into — a process-shared singleton both
// nodes' impls reach. DemoFsm_handlers.cc publishes *this into it on its
// first on_enter; this gate reads it. (Cross-TU declaration matches.)
theia::runtime::LocalRef<DemoFsm>& demo_fsm_ref() {
    static theia::runtime::LocalRef<DemoFsm> ref;
    return ref;
}

namespace {

template <typename Evt>
void forward_to_fsm(const char* node, const char* name, Evt evt) {
    auto& ref = demo_fsm_ref();
    if (!ref.valid()) {
        std::fprintf(stderr,
            "[%s] %s arrived before FSM wired — dropping\n", node, name);
        return;
    }
    std::fprintf(stderr, "[%s] %s → post_event to DemoFsm\n", node, name);
    theia::runtime::post_event(ref.target(), std::move(evt));
}

}  // namespace

void DemoFsmGate::init(DemoFsmGateState& /*s*/) {}
void DemoFsmGate::handle_info(const char* /*info*/, DemoFsmGateState& /*s*/) {}

void DemoFsmGate::handle_cast(const DemoStart& msg, DemoFsmGateState& /*s*/) {
    forward_to_fsm(kNodeName, "DemoStart", msg);
}

void DemoFsmGate::handle_cast(const DemoFinish& msg, DemoFsmGateState& /*s*/) {
    forward_to_fsm(kNodeName, "DemoFinish", msg);
}

void DemoFsmGate::handle_cast(const DemoReset& msg, DemoFsmGateState& /*s*/) {
    forward_to_fsm(kNodeName, "DemoReset", msg);
}

}  // namespace system_apps

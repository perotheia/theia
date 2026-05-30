// User handler bodies for SmGate — the SM's external-message front.
//
// HAND-OWNED (gen-app emits this once, then skips it without --force).
// Each lifecycle message the gate receives over TIPC is forwarded into
// the SmDaemon state machine IN-PROCESS via post_event(). The statem
// node itself takes no external messages — the gate is the only
// TIPC-reachable surface for FSM events, so the wire never drives the
// FSM directly.
//
// Cross-node reach: both SmDaemon and SmGate run as threads in this one
// sm process. main.cc constructs both, then publishes a
// LocalRef<SmDaemon> via set_sm_statem_ref(); post_event() enqueues onto
// the statem's mailbox, which is the thread-safe cross-thread path.

#include "lib/SmGate.hh"
#include "lib/SmDaemon.hh"

#include "GenStateM.hh"   // theia::runtime::post_event
#include "NodeRef.hh"     // theia::runtime::LocalRef

#include <cstdio>

namespace ara::sm {

// The statem peer, wired by main.cc after both nodes are constructed.
// Defined in main.cc (the only place that holds both instances).
theia::runtime::LocalRef<SmDaemon>& sm_statem_ref();

namespace {

// Forward a lifecycle event into the FSM if the statem peer is wired.
template <typename Evt>
void forward_to_statem(const char* node, const char* name, Evt evt) {
    auto& ref = sm_statem_ref();
    if (!ref.valid()) {
        std::fprintf(stderr,
            "[%s] %s arrived before statem wired — dropping\n", node, name);
        return;
    }
    std::fprintf(stderr, "[%s] %s → post_event to SmDaemon FSM\n", node, name);
    theia::runtime::post_event(ref.target(), std::move(evt));
}

}  // namespace

void SmGate::handle_cast(const SystemBoot& msg, SmGateState& /*s*/) {
    forward_to_statem(kNodeName, "SystemBoot", msg);
}

void SmGate::handle_cast(const StartupComplete& msg, SmGateState& /*s*/) {
    forward_to_statem(kNodeName, "StartupComplete", msg);
}

void SmGate::handle_cast(const ShutdownRequest& msg, SmGateState& /*s*/) {
    forward_to_statem(kNodeName, "ShutdownRequest", msg);
}

void SmGate::handle_cast(const UpdateRequest& msg, SmGateState& /*s*/) {
    forward_to_statem(kNodeName, "UpdateRequest", msg);
}

void SmGate::handle_cast(const UpdateComplete& msg, SmGateState& /*s*/) {
    forward_to_statem(kNodeName, "UpdateComplete", msg);
}

void SmGate::handle_cast(const RetryStartup& msg, SmGateState& /*s*/) {
    forward_to_statem(kNodeName, "RetryStartup", msg);
}

void SmGate::handle_cast(const PowerOff& msg, SmGateState& /*s*/) {
    forward_to_statem(kNodeName, "PowerOff", msg);
}

// OTP init/1 + local string handle_info — empty (the gate does no startup
// work and no self-tick; it only forwards inbound lifecycle casts into the
// FSM). Present to satisfy the lib decls gen-app emits for every node.
void SmGate::init(SmGateState& /*s*/) {}
void SmGate::handle_info(const char* /*info*/, SmGateState& /*s*/) {}

}  // namespace ara::sm

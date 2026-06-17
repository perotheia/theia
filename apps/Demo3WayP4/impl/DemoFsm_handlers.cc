// User handler bodies for DemoFsm (STATEM variant).
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Declarations are in lib/DemoFsm.hh.
//
// What lives here (user code):
//   - on_enter — fires after every committed FSM transition (and
//     once at init with new==old). Cast / send_after / log here.
//     Returns void, so transition_to() is COMPILE-TIME forbidden.
//   - handle_call overloads for any server-port operations — map
//     external requests onto FSM events or daemon state.
//
// Default bodies are no-ops with a stderr log so traffic is
// observable. Replace with real behaviour as the FC matures.

#include "lib/DemoFsm.hh"

#include "NodeRef.hh"   // theia::runtime::LocalRef — published for the gate

#include <cstdio>
#include <cstring>

namespace demo {

// The gate (DemoFsmGate_handlers.cc) post_event()s into this FSM via a
// shared LocalRef<DemoFsm>. We publish *this into it on the first on_enter
// (which the framework fires during start_statem, before any gate event can
// arrive). Defined in the gate's TU; declared here.
theia::runtime::LocalRef<DemoFsm>& demo_fsm_ref();

// on_enter — runs on the FSM thread AFTER every committed
// transition. The framework also fires it once at init with
// new_s == old_s == IDLE. SAFE to call
// cast/post_event/broadcast from here; UNSAFE to transition.
void DemoFsm::on_enter(DemoFsmState new_s,
                              DemoFsmState /*old_s*/,
                              DemoFsmData& d) {
    // Publish the FSM ref once so the gate can forward events in.
    if (!demo_fsm_ref().valid()) {
        demo_fsm_ref() = theia::runtime::LocalRef<DemoFsm>(*this);
    }
    // Mutate the FSM data (OTP `{State, Data}` Data term). The GenStateM base
    // snapshots `d` into the STATEM trace payload AFTER this returns, so rf
    // can `Assert Data visits=N reason=...` alongside the state name.
    d.visits++;
    const char* sn = state_name(new_s);
    std::strncpy(d.reason, sn, sizeof(d.reason) - 1);
    d.reason[sizeof(d.reason) - 1] = '\0';
    std::fprintf(stderr, "[%s] → %s (visits=%u)\n", kNodeName, sn, d.visits);
    // The STATEM trace (kind=5, from→to state + data) is emitted by the
    // GenStateM base on each transition — see GenStateM.hh. rf asserts on those.
}

// on_config_update — services/per casts platform.runtime.ConfigUpdated when the
// etcd-backed P4Config changes. The GenStateM base decodes + logs, then calls
// this hook. This demo FSM only reads config at boot, so the body is empty
// (the declaration is emitted by the lib because the node binds `config P4Config`).
void DemoFsm::on_config_update(
        const platform_runtime_ConfigUpdated& /*cfg*/,
        ::theia::runtime::GenStateMHolder<DemoFsmState, DemoFsmData>& /*h*/) {
}



}  // namespace demo

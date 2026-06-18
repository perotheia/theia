// User handler bodies for UcmFsm (STATEM variant).
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Declarations are in lib/UcmFsm.hh.
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

#include "lib/UcmFsm.hh"

#include "NodeRef.hh"     // theia::runtime::LocalRef — publish self to the gate

#include <chrono>
#include <cstdio>

namespace ara::ucm {

// The FSM ref UcmGate + UcmDaemon post_event() into. DEFINED in
// UcmGate_handlers.cc (impl-owned shared singleton); on_enter publishes `*this`
// into it on the FIRST entry (during start_statem(), before any event can land).
theia::runtime::LocalRef<UcmFsm>& ucm_fsm_ref();

namespace {
uint64_t now_ns_() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}
}  // namespace

// on_enter — runs on the FSM thread AFTER every committed transition (and once
// at init with new==old==IDLE). UcmFsmData IS UcmProgress, so stamping `d`
// updates BOTH the persisted progress and the broadcast payload. SAFE to
// broadcast/log; UNSAFE to transition.
void UcmFsm::on_enter(UcmFsmState new_s,
                              UcmFsmState /*old_s*/,
                              UcmFsmData& d) {
    // Publish self to the gate on first entry (idempotent later). The initial
    // IDLE entry runs during start_statem(), so the ref is wired before the
    // gate/daemon can post an event.
    if (!ucm_fsm_ref().valid()) {
        ucm_fsm_ref() = theia::runtime::LocalRef<UcmFsm>(*this);
    }

    // The C++ UcmFsmState enum is dense 0..8 matching the wire UcmState order,
    // so the cast is exact here.
    d.state = static_cast<system_services_ucm_UcmState>(new_s);
    d.ts_ns = now_ns_();

    this->log().info(std::string("→ ") + UcmFsm::state_name(new_s));

    // Fan the progress out to every UcmProgressStream subscriber (com UcmView /
    // the fleet watch the update walk its lifecycle). The release-dir EXECUTION
    // per state is UcmGate's job (Phase 3) — this node only owns the FSM + the
    // progress broadcast/persist.
    broadcast_progress_progress(d);
}



}  // namespace ara::ucm

// User handler bodies for FunctionGroupSm (STATEM variant).
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Declarations are in lib/FunctionGroupSm.hh.
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

#include "lib/FunctionGroupSm.hh"

#include "NodeRef.hh"   // theia::runtime::LocalRef — publish self to FgGate

#include <chrono>
#include <cstdio>
#include <string>

namespace ara::sm {

// The FG FSM's LocalRef, defined in FgGate_handlers.cc. on_enter publishes self
// into it on the first (initial FG_OFF) entry, so FgGate can post_event before
// any wire event arrives — same publish-on-first-entry idiom as SmDaemon.
theia::runtime::LocalRef<FunctionGroupSm>& fg_statem_ref();

namespace {
uint64_t now_ns_() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
}  // namespace

// on_enter — runs on the FSM thread AFTER every committed transition (and once
// at init with new_s == old_s == FG_OFF). SAFE to cast/post_event/broadcast
// here; UNSAFE to transition. FunctionGroupSmData IS FgStatusMsg (the .art
// `data FgStatusMsg`), so mutating `d` updates the broadcast payload + the FSM's
// persistent data in one move — same shape as SmDaemon::on_enter.
void FunctionGroupSm::on_enter(FunctionGroupSmState new_s,
                              FunctionGroupSmState /*old_s*/,
                              FunctionGroupSmData& d) {
    // Publish self to the gate on first entry (idempotent on later transitions).
    // The initial FG_OFF entry runs during start_statem(), so the ref is wired
    // before FgGate could receive a forwardable event.
    if (!fg_statem_ref().valid()) {
        fg_statem_ref() = theia::runtime::LocalRef<FunctionGroupSm>(*this);
    }

    d.state = static_cast<system_services_sm_FgState>(new_s);
    d.ts_ns = now_ns_();
    // `d.fg` identifies WHICH Function Group; one FSM instance (MachineFG=0) for
    // now — a multi-instance fan-out would set this per group.

    static const char* names[] = {
        "FG_OFF", "FG_STARTUP", "FG_RUNNING", "FG_SHUTDOWN", "FG_RESTART",
    };
    const auto idx = static_cast<std::size_t>(new_s);
    this->log().info(std::string("FG → ") +
        (idx < sizeof(names)/sizeof(names[0]) ? names[idx] : "?") + " @ " +
        std::to_string(d.ts_ns));

    // Broadcast the new FG state to every FgStateStream subscriber (the GUI,
    // tdb, a mode-policy actor). The lib template snapshots subscribers under
    // the lock + invokes outside it, so a slow subscriber can't stall the FSM.
    broadcast_broadcast_fg_state(d);

    // NOTE: the SM→EM EXECUTE drive (stop/start the FG's sub-tree via the
    // supervisor) now lives in FgGate — the MULTI-FG authority — which tracks a
    // per-FG state map + drives each FG's sub-tree by msg.fg. This statem node is
    // the MachineFG(0) REFERENCE FSM: it owns the canonical STATEM trace + the
    // FgStateStream broadcast, but no longer drives EM itself (the gate does, so
    // we don't double stop/start). See FgGate_handlers.cc fg_transition().
}

}  // namespace ara::sm

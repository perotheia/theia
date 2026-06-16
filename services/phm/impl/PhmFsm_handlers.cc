// User handler bodies for PhmFsm (STATEM variant) — the per-entity fault FSM.
//
// HAND-OWNED (gen-app emits this once, then skips it without --force).
// PhmFsm runs the OK→WARNING→DEGRADED→FAILED fault state machine. PhmGate does
// the threshold counting and post_event()s FaultObserved/FaultEscalate/
// FaultCleared into this FSM IN-PROCESS (via phm_fsm_ref()); the FSM owns the
// transitions + the on_enter escalation. On DEGRADED/FAILED it casts a
// PhmHealthStatus → State Management (SM's FgGate, the comm-matrix phm→sm edge,
// which translates it to FgDegraded) and emits a PhmFaultEvent on the DTC
// stream (future ara::diag). PhmFsmData IS PhmHealthStatus (the .art `data
// PhmHealthStatus`), so mutating `d` updates the broadcast payload + the FSM's
// persistent data in one move — same shape as SM's FunctionGroupSm::on_enter.

#include "lib/PhmFsm.hh"

#include "NodeRef.hh"   // theia::runtime::LocalRef + cast(self, msg, TipcAddr)

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace ara::phm {

// The PHM fault FSM's LocalRef, shared with PhmGate_handlers.cc. on_enter
// publishes self into it on the first (initial OK) entry — wired before any
// wire event can be forwarded. Same publish-on-first-entry idiom as SM.
theia::runtime::LocalRef<PhmFsm>& phm_fsm_ref() {
    static theia::runtime::LocalRef<PhmFsm> ref;
    return ref;
}

namespace {

uint64_t now_ns_() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// SM's FgGate — the comm-matrix phm→sm escalation target. Its TIPC address is
// the .art-declared `node atomic FgGate { tipc type=0x8001004D instance=0 }`
// in services/sm/system/sm/package.art. SM's FgGate imports
// system.services.phm.PhmHealthStatus so its register_cast service_id matches
// the one PHM stamps (djb2 of system_services_phm_PhmHealthStatus). There is no
// .art `connect` arrow phm→sm (cross-FC wiring is by well-known address), so
// this is a direct addressed cast rather than a netgraph:: peer.
constexpr uint32_t kSmFgGateTipcType     = 0x8001004Du;
constexpr uint32_t kSmFgGateTipcInstance = 0u;

}  // namespace

// on_enter — runs on the FSM thread AFTER every committed transition (and once
// at init with new_s == old_s == OK). SAFE to cast/broadcast here; UNSAFE to
// transition.
void PhmFsm::on_enter(PhmFsmState new_s,
                      PhmFsmState /*old_s*/,
                      PhmFsmData& d) {
    // Publish self to the gate on first entry (idempotent on later
    // transitions). The initial OK entry runs during start_statem(), so the
    // ref is wired before PhmGate could receive a forwardable event.
    if (!phm_fsm_ref().valid()) {
        phm_fsm_ref() = theia::runtime::LocalRef<PhmFsm>(*this);
    }

    // PhmFsmData IS PhmHealthStatus — set the level to the new state's ordinal
    // (OK=0..FAILED=3, matching HealthLevel) + a fresh timestamp. `entity` /
    // `detail` carry whatever the last transition left (the gate selects the
    // entity in a richer build; v1 is the platform-wide worst-case verdict).
    d.level = static_cast<system_services_phm_HealthLevel>(
        static_cast<uint32_t>(new_s));
    d.ts_ns = now_ns_();

    static const char* names[] = {"OK", "WARNING", "DEGRADED", "FAILED"};
    const auto idx = static_cast<std::size_t>(new_s);
    this->log().info(std::string("fault → ") +
        (idx < sizeof(names)/sizeof(names[0]) ? names[idx] : "?") + " @ " +
        std::to_string(d.ts_ns));

    // Broadcast the health update to every PhmHealthStream subscriber (the GUI,
    // tdb, an observer) on EVERY transition — observers see OK/WARNING too.
    broadcast_to_sm_health(d);

    // Escalation to State Management — only at DEGRADED (2) or FAILED (3). Below
    // that, the broadcast above is the only notification (no FG action needed).
    // PHM informs; SM's FgGate decides (it translates a DEGRADED+ verdict into
    // FgDegraded for the affected Function Group).
    if (new_s == PhmFsmState::DEGRADED || new_s == PhmFsmState::FAILED) {
        theia::runtime::cast(*this, d,
            theia::runtime::TipcAddr{kSmFgGateTipcType, kSmFgGateTipcInstance,
                                     "fg_gate"});

        // DTC-like fault record → future ara::diag (PhmDtcStream). Distinct from
        // the SM escalation: the persistent fault-history surface. Broadcast to
        // the in-process dtc_out subscriber list (no remote diag FC yet).
        PhmFaultEvent fault = system_services_phm_PhmFaultEvent_init_zero;
        std::snprintf(fault.entity, sizeof(fault.entity), "%s", d.entity);
        fault.level = d.level;
        fault.kind  = d.kind;
        fault.ts_ns = d.ts_ns;
        std::snprintf(fault.detail, sizeof(fault.detail), "%s", d.detail);
        broadcast_dtc_out_fault(fault);
    }
}

}  // namespace ara::phm

// User handler bodies for UcmDaemon — the UCM agent's ctl FRONT.
//
// HAND-OWNED (gen-app emits once, then skips without --force).
//
// UcmDaemon is the external-facing control node: the fleet manager / com forward
// a PackageManifest via UpdateCtl.RequestUpdate. UcmDaemon validates it, stashes
// it as the in-flight manifest, and post_event()s EvStartUpdate into UcmFsm
// IN-PROCESS via a LocalRef<UcmFsm> — the same gate→fsm idiom as SmDaemon and
// PhmGate. The release-dir EXECUTION (download/verify/stage/switch/rollback)
// lives in UcmGate (the FSM's per-state work); UcmDaemon only kicks it off.
// It also subscribes to SmStateStream so a future build can refuse updates
// outside RUNNING.

#include "lib/UcmDaemon.hh"
#include "lib/UcmFsm.hh"        // the agent FSM we post_event into

#include "GenStateM.hh"         // theia::runtime::post_event
#include "NodeRef.hh"           // theia::runtime::LocalRef

#include <cstring>
#include <string>

namespace ara::ucm {

// The agent FSM peer. IMPL-OWNED shared singleton, DEFINED in UcmGate_handlers.cc
// (UcmGate is the gate that drives every other lifecycle event into it).
// UcmFsm::on_enter publishes *this into it on first entry, so by the time a
// RequestUpdate lands the ref is valid. Forward-declared here.
theia::runtime::LocalRef<UcmFsm>& ucm_fsm_ref();

// The most recent manifest accepted by RequestUpdate — UcmGate reads it to know
// what to download/stage. IMPL-owned, defined in UcmGate_handlers.cc. (Nanopb C
// type; PackageManifest is the UcmDaemon.hh alias for the same struct.)
system_services_ucm_PackageManifest& ucm_pending_manifest();

void UcmDaemon::init(UcmDaemonState& /*s*/) {
    this->log().info("ucm agent ctl up — UpdateCtl.RequestUpdate ready");
}

void UcmDaemon::handle_info(const char* /*info*/, UcmDaemonState& /*s*/) {
}

// RequestUpdate — the fleet/VUCM campaign lands here (via com gRPC UcmView).
// Stash the manifest + kick the agent FSM with EvStartUpdate. The heavy lifting
// is UcmGate's; this returns immediately with an accept/reject status.
UcmReply UcmDaemon::handle_call(
        const PackageManifest& req,
        UcmDaemonState& /*s*/) {
    UcmReply reply = system_services_ucm_UcmReply_init_zero;

    if (!req.version[0]) {
        this->log().warn("RequestUpdate rejected — empty version");
        reply.status = 1;   // reject
        return reply;
    }
    // nanopb doubles the enum name (proto enum UpdateKind, values UpdateKind_*).
    this->log().info(std::string("RequestUpdate: name=") + req.name +
        " version=" + req.version +
        " kind=" + (req.kind == system_services_ucm_UpdateKind_UpdateKind_UK_CONFIG
                    ? "CONFIG" : "SOFTWARE") +
        " scope=" + (req.scope == system_services_ucm_UpdateScope_UpdateScope_US_PARTIAL
                     ? "PARTIAL" : "FULL"));

    // Hand the manifest to UcmGate (the executor) + start the FSM.
    ucm_pending_manifest() = req;
    auto& ref = ucm_fsm_ref();
    if (!ref.valid()) {
        this->log().warn("RequestUpdate: UcmFsm not wired yet — try again shortly");
        reply.status = 2;   // not-ready
        return reply;
    }
    theia::runtime::post_event(ref.target(), EvStartUpdate{});
    reply.status = 0;       // accepted
    return reply;
}

}  // namespace ara::ucm

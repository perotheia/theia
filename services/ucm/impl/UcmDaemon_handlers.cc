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
#include "lib/UcmGate.hh"       // the executor gate we cast EvStartUpdate to

#include "GenStateM.hh"         // theia::runtime::post_event
#include "NodeRef.hh"           // theia::runtime::LocalRef + cast

#include <cstring>
#include <string>

namespace ara::ucm {

// The agent FSM + gate peers. IMPL-OWNED shared singletons, DEFINED in
// UcmGate_handlers.cc. UcmFsm::on_enter / UcmGate::init publish *this on first
// entry, so by the time a RequestUpdate lands both refs are valid. EvStartUpdate
// goes to BOTH: the FSM (IDLE→DOWNLOADED state + broadcast) AND the gate (which
// does the download work + drives the rest of the chain). Forward-declared here.
theia::runtime::LocalRef<UcmFsm>& ucm_fsm_ref();
theia::runtime::LocalRef<UcmGate>& ucm_gate_ref();

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

    // Hand the manifest to UcmGate (the executor) + start the update: advance
    // the FSM (IDLE→DOWNLOADED) AND kick the gate (do the download + drive the
    // chain). Both must be wired (published on their first node entry).
    ucm_pending_manifest() = req;
    auto& fsm = ucm_fsm_ref();
    auto& gate = ucm_gate_ref();
    if (!fsm.valid() || !gate.valid()) {
        this->log().warn("RequestUpdate: agent not wired yet — try again shortly");
        reply.status = 2;   // not-ready
        return reply;
    }
    theia::runtime::post_event(fsm.target(), EvStartUpdate{});  // state → DOWNLOADED
    theia::runtime::cast(gate, EvStartUpdate{});                // gate does the work
    reply.status = 0;       // accepted
    return reply;
}

}  // namespace ara::ucm

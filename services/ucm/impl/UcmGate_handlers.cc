// User handler bodies for UcmGate.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/UcmGate.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/UcmGate.hh"
#include "lib/UcmFsm.hh"        // the agent FSM the gate post_event()s into

#include "GenStateM.hh"         // theia::runtime::post_event
#include "NodeRef.hh"           // theia::runtime::LocalRef

#include <cstdio>

namespace ara::ucm {

// ---- IMPL-owned shared singletons (the gate→fsm wiring) -------------------
// The agent FSM peer. UcmFsm::on_enter publishes itself here on first entry;
// UcmDaemon (RequestUpdate) + UcmGate (every lifecycle step) post_event into it.
theia::runtime::LocalRef<UcmFsm>& ucm_fsm_ref() {
    static theia::runtime::LocalRef<UcmFsm> ref;
    return ref;
}

// The in-flight package manifest UcmDaemon accepted. UcmGate reads it to know
// what to download/stage/switch. One update in flight at a time. (Use the
// nanopb C type directly — the PackageManifest alias lives in UcmDaemon.hh, not
// UcmGate's lib, since the gate's ports don't carry it.)
system_services_ucm_PackageManifest& ucm_pending_manifest() {
    static system_services_ucm_PackageManifest m =
        system_services_ucm_PackageManifest_init_zero;
    return m;
}

namespace {
// Post an Ev* into the agent FSM if it's wired (first ticks can race the FSM's
// initial-entry publish — dropping then is harmless; the step re-fires).
template <typename Evt>
void to_fsm(const char* name, Evt evt) {
    auto& ref = ucm_fsm_ref();
    if (!ref.valid()) {
        std::fprintf(stderr, "[ucm_gate] %s before FSM wired — dropping\n", name);
        return;
    }
    theia::runtime::post_event(ref.target(), std::move(evt));
}
}  // namespace

// ---- OTP init/1 — runs once on the node thread after start().
void UcmGate::init(UcmGateState& /*s*/) {
    this->log().info("ucm gate up — release-dir executor + health gate ready");
}

// ---- string handle_info — the post_info()/send_after() tick path.
void UcmGate::handle_info(const char* /*info*/, UcmGateState& /*s*/) {
}

// ---- config update — services/per casts ConfigUpdated when this node's
//      etcd-backed `config UcmConfig` changes. The GenServer base decoded
//      the envelope + logged; apply the typed config here (ParseFromString
//      cfg.config into UcmConfig, honor the changed mask). Empty default —
//      a node that only reads config at boot leaves this as-is.
void UcmGate::on_config_update(
        const platform_runtime_ConfigUpdated& /*cfg*/,
        UcmGateState& /*s*/) {
}



void UcmGate::handle_cast(const EvStartUpdate& /*msg*/,
                                 UcmGateState& /*s*/) {
    // TODO: react to EvStartUpdate (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received EvStartUpdate\n",
                 kNodeName);
}

void UcmGate::handle_cast(const EvDownloaded& /*msg*/,
                                 UcmGateState& /*s*/) {
    // TODO: react to EvDownloaded (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received EvDownloaded\n",
                 kNodeName);
}

void UcmGate::handle_cast(const EvValidated& /*msg*/,
                                 UcmGateState& /*s*/) {
    // TODO: react to EvValidated (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received EvValidated\n",
                 kNodeName);
}

void UcmGate::handle_cast(const EvStaged& /*msg*/,
                                 UcmGateState& /*s*/) {
    // TODO: react to EvStaged (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received EvStaged\n",
                 kNodeName);
}

void UcmGate::handle_cast(const EvInstalled& /*msg*/,
                                 UcmGateState& /*s*/) {
    // TODO: react to EvInstalled (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received EvInstalled\n",
                 kNodeName);
}

void UcmGate::handle_cast(const EvRestarted& /*msg*/,
                                 UcmGateState& /*s*/) {
    // TODO: react to EvRestarted (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received EvRestarted\n",
                 kNodeName);
}

void UcmGate::handle_cast(const EvVerified& /*msg*/,
                                 UcmGateState& /*s*/) {
    // TODO: react to EvVerified (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received EvVerified\n",
                 kNodeName);
}

void UcmGate::handle_cast(const EvFailed& /*msg*/,
                                 UcmGateState& /*s*/) {
    // TODO: react to EvFailed (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received EvFailed\n",
                 kNodeName);
}

void UcmGate::handle_cast(const EvRolledBack& /*msg*/,
                                 UcmGateState& /*s*/) {
    // TODO: react to EvRolledBack (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received EvRolledBack\n",
                 kNodeName);
}

void UcmGate::handle_cast(const PhmHealthStatus& /*msg*/,
                                 UcmGateState& /*s*/) {
    // TODO: react to PhmHealthStatus (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received PhmHealthStatus\n",
                 kNodeName);
}




}  // namespace ara::ucm

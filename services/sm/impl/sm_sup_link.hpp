// sm_sup_link — FunctionGroupSm's RemoteRef link to the supervisor's
// SupervisorControlIf (SupervisorCtl @0x80020001). APP-OWNED.
//
// This is the SM→EM EXECUTE path (State-Management.md §4): SM DECIDES the desired
// Function-Group state; the supervisor (Theia's Execution Management) REALISES it
// by starting/stopping the FG's mapped supervision sub-tree. We reuse the
// supervisor's existing TerminateChild(no_restart) / StartChild primitives — no
// new supervisor op (avoids the supervisor.proto regen-tag hazard).
//
// FunctionGroupSm is a statem; on_enter must NOT block, so the cast is DEFERRED
// onto the node's own mailbox (post_info) and the BLOCKING RemoteRef call runs in
// handle_info — the nested-cross-FC-call-from-a-handler pattern (proven by the
// runtime case_nested_remoteref_call test; same shape as diag's crypto_link).
// Own TipcMux reply pump, lazily connected.

#pragma once

#include "RemoteCodec.hh"
#include "TipcMux.hh"
#include "NodeRef.hh"

#include "system/supervisor/supervisor.pb.h"   // ChildSelector / StartChildRequest / ControlReply

#include <cstring>
#include <string>
#include <vector>

// The supervisor control types crossing the wire need a RemoteCodec so RemoteRef
// dispatches them to the same service_id the supervisor's register_call uses.
THEIA_DECLARE_REMOTE_CODEC(system_supervisor_ChildSelector)
THEIA_DECLARE_REMOTE_CODEC(system_supervisor_StartChildRequest)
THEIA_DECLARE_REMOTE_CODEC(system_supervisor_ControlReply)

namespace ara::sm {

struct SmSupLink {
    struct SupTag { static constexpr const char* kNodeName = "supervisor_ctl"; };
    using Ref = theia::runtime::RemoteRef<SupTag, 0x80020001u, 0u>;

    theia::runtime::TipcMux mux;
    Ref                     ref;
    bool                    up = false;

    static SmSupLink& instance() { static SmSupLink l; return l; }

    bool ensure() {
        if (up) return true;
        if (!ref.connect_instance(0, /*timeout_ms=*/1500)) return false;
        mux.watch_remote_ref(ref);
        mux.start();
        up = true;
        return true;
    }

    // Stop a supervision sub-tree (the FG's mapped node), HOLDING it down
    // (no_restart) so the supervisor's restart policy doesn't fight SM's
    // desired-OFF. Returns true on a reply.
    bool stop_subtree(const std::string& name) {
        if (!ensure()) return false;
        system_supervisor_ChildSelector sel = system_supervisor_ChildSelector_init_zero;
        std::snprintf(sel.name, sizeof(sel.name), "%s", name.c_str());
        sel.no_restart = true;     // terminate-and-hold (SM owns desired state)
        auto r = theia::runtime::call<system_supervisor_ControlReply>(
            ref, sel, /*act=*/0, /*timeout_ms=*/3000);
        return r.tag == theia::runtime::CallTag::Reply;
    }

    // Start (clear the hold on) a supervision sub-tree — bring the FG back up.
    bool start_subtree(const std::string& name) {
        if (!ensure()) return false;
        system_supervisor_StartChildRequest req =
            system_supervisor_StartChildRequest_init_zero;
        req.has_spec = true;       // optional submessage — nanopb gates encode on it
        std::snprintf(req.spec.name, sizeof(req.spec.name), "%s", name.c_str());
        auto r = theia::runtime::call<system_supervisor_ControlReply>(
            ref, req, /*act=*/0, /*timeout_ms=*/3000);
        return r.tag == theia::runtime::CallTag::Reply;
    }
};

// The Function Groups SM manages, each mapped to a supervisor supervision
// sub-tree (State-Management.md §1). MULTI-FG: FgGate tracks per-FG state +
// drives each FG's sub-tree independently. MachineFG is the whole machine
// (root); the rest are layer FGs. Keep the ids stable — PhmHealthStatus.fg +
// the FgLifecycleIn events address an FG by this ordinal.
enum FgId : uint32_t {
    FG_MACHINE  = 0,   // the whole machine (root) — used by the demo MachineFG FSM
    FG_PLATFORM = 1,   // core_sup — the platform layer (com/per/sm/crypto/…)
    FG_NETWORK  = 2,   // network_sup — nm/osi/idsm/diag/tsync
    FG_HOSTSVC  = 3,   // host_svc_sup
    FG_APP      = 4,   // app_sup — the vendor application layer
};

// FG id → the supervision sub-tree it maps to. An unknown id falls back to
// app_sup (the safest thing to stop on an unrecognised degrade).
inline std::string fg_subtree(uint32_t fg) {
    switch (fg) {
        case FG_MACHINE:  return "root";
        case FG_PLATFORM: return "core_sup";
        case FG_NETWORK:  return "network_sup";
        case FG_HOSTSVC:  return "host_svc_sup";
        case FG_APP:      return "app_sup";
        default:          return "app_sup";
    }
}

// The FGs SM brings up/down as a set (for the broadcast-all-FGs paths). Excludes
// FG_MACHINE (that's the aggregate, not a stoppable sub-tree on its own).
inline const std::vector<uint32_t>& managed_fgs() {
    static const std::vector<uint32_t> v = {
        FG_PLATFORM, FG_NETWORK, FG_HOSTSVC, FG_APP };
    return v;
}

}  // namespace ara::sm

// phm_link — the UdsRouter's RemoteRef link to services/phm's PhmCtlIf
// (PhmGate @0x80010075), for UDS 0x19 ReadDTCInformation. APP-OWNED.
//
// 0x19 reads the vehicle's fault memory. In Theia, PHM is the fault authority:
// GetHealthStatus returns the worst level + a degraded count across all tracked
// entities. We map that summary onto a UDS DTC record (a real build subscribes
// to PhmDtcStream for per-entity PhmFaultEvents → per-DTC records; v1 derives one
// aggregate DTC from the health summary). Same RemoteRef+pump shape as
// crypto_link; blocking call from handle_call (the UDS request/reply is sync).

#pragma once

#include "RemoteCodec.hh"
#include "TipcMux.hh"
#include "NodeRef.hh"

#include "system/services/phm/phm.pb.h"   // PhmStatusReq / PhmStatusMsg

THEIA_DECLARE_REMOTE_CODEC(system_services_phm_PhmStatusReq)
THEIA_DECLARE_REMOTE_CODEC(system_services_phm_PhmStatusMsg)

namespace ara::diag {

struct PhmLink {
    struct PhmTag { static constexpr const char* kNodeName = "phm_gate"; };
    using Ref = theia::runtime::RemoteRef<PhmTag, 0x80010075u, 0u>;

    theia::runtime::TipcMux mux;
    Ref                     ref;
    bool                    up = false;

    static PhmLink& instance() { static PhmLink l; return l; }

    bool ensure() {
        if (up) return true;
        if (!ref.connect_instance(0, /*timeout_ms=*/1500)) return false;
        mux.watch_remote_ref(ref);
        mux.start();
        up = true;
        return true;
    }

    // GetHealthStatus → worst level (0=OK..3=FAILED) + degraded count. Returns
    // false on transport failure (the caller reports "no DTCs", fail-safe).
    bool health(uint32_t& worst, uint32_t& n_entities, uint32_t& n_degraded) {
        if (!ensure()) return false;
        system_services_phm_PhmStatusReq req = system_services_phm_PhmStatusReq_init_zero;
        auto r = theia::runtime::call<system_services_phm_PhmStatusMsg>(
            ref, req, /*act=*/0, /*timeout_ms=*/2000);
        if (r.tag != theia::runtime::CallTag::Reply) return false;
        worst       = static_cast<uint32_t>(r.reply.worst);
        n_entities  = r.reply.n_entities;
        n_degraded  = r.reply.n_degraded;
        return true;
    }
};

}  // namespace ara::diag

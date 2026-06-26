// vucm_link — com's RemoteRef link to services/vucm's VucmGate (the L4-B vehicle
// campaign orchestrator), so com can proxy V-UCM's CheckForCampaign + the
// campaign status over gRPC (VucmView) for the fleet operator (GS). Mirrors
// ucm_link exactly: the nanopb vucm structs + the RemoteRef/TipcMux live ONLY in
// vucm_link.cc, so the libprotobuf gRPC edge (ComGrpcProxy_handlers.cc) never
// meets the nanopb vucm headers.
//
// VucmGate is at TIPC 0x8001005E/0 (VucmCtlIf.CheckForCampaign /
// GetCampaignStatus). The campaign progress sample comes from the same call
// (GetCampaignStatus → CampaignProgress); the GS poll reads it.

#pragma once

#include <cstdint>
#include <string>

namespace services_com {

// A campaign request in primitives (the gRPC edge → nanopb CampaignRequest).
struct VucmCampaignReq {
    std::string campaign_id;     // the Mender deployment id
    std::string version;         // target release, e.g. "2026.08"
    uint32_t    scope = 0;       // UpdateScope: 0=FULL 1=PARTIAL
};

// One decoded CampaignProgress sample, in primitives.
struct VucmCampaignSample {
    uint32_t    state = 0;       // CampaignState ordinal (wire: 0 IDLE..6 ROLLBACK, 7 CONFIRMING)
    std::string campaign_id;
    std::string version;
    std::string detail;
    uint64_t    ts_ns = 0;
    bool        valid = false;   // false = no sample yet
};

// Singleton link to VucmGate. Opened by ComGrpcProxy::do_start, torn down by
// do_stop. Thread-safe.
class VucmLink {
public:
    static VucmLink& instance();

    // Connect the RemoteRef (TIPC 0x8001005E/0) + start the reply pump. Returns
    // false if vucm isn't reachable (a coordinator-only board has it; a
    // worker-only board doesn't — the link just stays !connected there).
    bool start(int connect_timeout_ms = 3000);
    void stop();
    bool connected() const;

    // CheckForCampaign(req) → CampaignReply.accepted (1=queued) + state. Returns
    // false on transport error / timeout / not-connected.
    bool check_for_campaign(const VucmCampaignReq& req, uint32_t& accepted_out,
                            uint32_t& state_out, int timeout_ms = 6000);

    // GetCampaignStatus → the current CampaignProgress (the aggregate-barrier
    // state the GS fleet view shows). valid=false until the first reply.
    VucmCampaignSample status(int timeout_ms = 4000);

    // L4-C operator commit/rollback (step 7): once V-UCM reports AWAITING_COMMIT,
    // commit (rollback=false → fan Confirm) or roll back (rollback=true → fan
    // Cancel) the campaign. → DecisionReply.accepted (1 if applied) + state.
    bool decide(const std::string& campaign_id, bool rollback,
                uint32_t& accepted_out, uint32_t& state_out, int timeout_ms = 8000);

private:
    VucmLink();
    ~VucmLink();
    VucmLink(const VucmLink&)            = delete;
    VucmLink& operator=(const VucmLink&) = delete;

    struct Impl;
    Impl* impl_;   // pimpl so this header stays free of runtime/TIPC types
};

}  // namespace services_com

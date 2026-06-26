// vucm_link implementation — RemoteRef call to VucmGate (CheckForCampaign /
// GetCampaignStatus). The nanopb vucm structs live ONLY here, confined to this TU
// so the libprotobuf gRPC edge in ComGrpcProxy_handlers.cc stays free of them.
// Mirrors ucm_link.cc, minus the PG fold — the campaign status is a direct call
// (GetCampaignStatus), so com polls it on demand rather than folding a broadcast.

#include "impl/vucm_link.hpp"

#include "NodeRef.hh"
#include "TipcMux.hh"
#include "system/services/vucm/vucm.pb.h"   // nanopb vucm structs
#include "RemoteCodec.hh"
#include <pb_decode.h>

// The two VucmCtlIf request/reply pairs com calls. Declared here (this TU only).
THEIA_DECLARE_REMOTE_CODEC(system_services_vucm_CampaignRequest)
THEIA_DECLARE_REMOTE_CODEC(system_services_vucm_CampaignReply)
THEIA_DECLARE_REMOTE_CODEC(system_services_vucm_CampaignStatusReq)
THEIA_DECLARE_REMOTE_CODEC(system_services_vucm_CampaignProgress)
THEIA_DECLARE_REMOTE_CODEC(system_services_vucm_CommitRequest)
THEIA_DECLARE_REMOTE_CODEC(system_services_vucm_RollbackRequest)
THEIA_DECLARE_REMOTE_CODEC(system_services_vucm_DecisionReply)

#include <cstdio>
#include <mutex>

namespace services_com {

namespace {
// VucmGate (services/vucm) — the L4-B campaign orchestrator, VucmCtlIf.
constexpr uint32_t kVucmGateTipcType     = 0x8001005Eu;
constexpr uint32_t kVucmGateTipcInstance = 0u;

struct VucmGateTag { static constexpr const char* kNodeName = "vucm_gate"; };
using VucmRef =
    theia::runtime::RemoteRef<VucmGateTag, kVucmGateTipcType, kVucmGateTipcInstance>;

void set_str(char* dst, size_t cap, const std::string& s) {
    std::snprintf(dst, cap, "%s", s.c_str());
}
std::string from_str(const char* s) { return std::string(s ? s : ""); }
}  // namespace

struct VucmLink::Impl {
    theia::runtime::TipcMux mux;
    VucmRef                 ref;
    bool                    started = false;
    std::mutex              call_mu;
};

VucmLink::VucmLink() : impl_(new Impl()) {}
VucmLink::~VucmLink() { stop(); delete impl_; }

VucmLink& VucmLink::instance() {
    static VucmLink s;
    return s;
}

bool VucmLink::start(int connect_timeout_ms) {
    if (impl_->started) return true;
    // A worker-only board (no vucm) just fails to connect — leave !started, the
    // gRPC handlers report unavailable. Only the coordinator board has VucmGate.
    if (!impl_->ref.connect(connect_timeout_ms)) return false;
    impl_->mux.watch_remote_ref(impl_->ref);
    impl_->mux.start();
    impl_->started = true;
    return true;
}

void VucmLink::stop() {
    if (!impl_->started) return;
    impl_->mux.stop();
    impl_->started = false;
}

bool VucmLink::connected() const { return impl_->started; }

bool VucmLink::check_for_campaign(const VucmCampaignReq& req, uint32_t& accepted_out,
                                  uint32_t& state_out, int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);

    system_services_vucm_CampaignRequest c =
        system_services_vucm_CampaignRequest_init_zero;
    set_str(c.campaign_id, sizeof(c.campaign_id), req.campaign_id);
    set_str(c.version, sizeof(c.version), req.version);
    c.scope = static_cast<system_services_ucm_UpdateScope>(req.scope);

    auto result = theia::runtime::call<system_services_vucm_CampaignReply>(
        impl_->ref, c, /*act=*/0, timeout_ms);
    if (result.tag != theia::runtime::CallTag::Reply) return false;
    accepted_out = result.reply.accepted;
    state_out    = static_cast<uint32_t>(result.reply.state);
    return true;
}

bool VucmLink::decide(const std::string& campaign_id, bool rollback,
                      uint32_t& accepted_out, uint32_t& state_out, int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    if (rollback) {
        system_services_vucm_RollbackRequest req =
            system_services_vucm_RollbackRequest_init_zero;
        set_str(req.campaign_id, sizeof(req.campaign_id), campaign_id);
        auto r = theia::runtime::call<system_services_vucm_DecisionReply>(
            impl_->ref, req, /*act=*/0, timeout_ms);
        if (r.tag != theia::runtime::CallTag::Reply) return false;
        accepted_out = r.reply.accepted; state_out = static_cast<uint32_t>(r.reply.state);
    } else {
        system_services_vucm_CommitRequest req =
            system_services_vucm_CommitRequest_init_zero;
        set_str(req.campaign_id, sizeof(req.campaign_id), campaign_id);
        auto r = theia::runtime::call<system_services_vucm_DecisionReply>(
            impl_->ref, req, /*act=*/0, timeout_ms);
        if (r.tag != theia::runtime::CallTag::Reply) return false;
        accepted_out = r.reply.accepted; state_out = static_cast<uint32_t>(r.reply.state);
    }
    return true;
}

VucmCampaignSample VucmLink::status(int timeout_ms) {
    VucmCampaignSample s;
    if (!impl_->started) return s;
    std::lock_guard<std::mutex> lk(impl_->call_mu);

    system_services_vucm_CampaignStatusReq req =
        system_services_vucm_CampaignStatusReq_init_zero;
    auto result = theia::runtime::call<system_services_vucm_CampaignProgress>(
        impl_->ref, req, /*act=*/0, timeout_ms);
    if (result.tag != theia::runtime::CallTag::Reply) return s;
    s.state       = static_cast<uint32_t>(result.reply.state);
    s.campaign_id = from_str(result.reply.campaign_id);
    s.version     = from_str(result.reply.version);
    s.detail      = from_str(result.reply.detail);
    s.ts_ns       = result.reply.ts_ns;
    s.valid       = true;
    return s;
}

}  // namespace services_com

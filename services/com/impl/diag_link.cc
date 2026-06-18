// diag_link implementation — RemoteRef + reply-pump TipcMux to UdsRouter.
//
// The nanopb diag structs live ONLY here (diag_codecs.hh + diag.pb.h), confined
// to this TU so the libprotobuf gRPC edge in ComGrpcProxy_handlers.cc stays free
// of them. Mirrors nm_link.cc.

#include "impl/diag_link.hpp"

#include "NodeRef.hh"
#include "TipcMux.hh"
#include "system/services/diag/diag.pb.h"   // nanopb diag structs
#include "lib/diag_codecs.hh"               // RemoteCodec specializations

#include <algorithm>
#include <cstring>
#include <mutex>

namespace services_com {

namespace {

// UdsRouter (services/diag) — the UDS dispatch surface.
constexpr uint32_t kDiagTipcType     = 0x80010018u;
constexpr uint32_t kDiagTipcInstance = 0u;

struct UdsRouterTag {
    static constexpr const char* kNodeName = "uds_router";
};
using DiagRef =
    theia::runtime::RemoteRef<UdsRouterTag, kDiagTipcType, kDiagTipcInstance>;

}  // namespace

struct DiagLink::Impl {
    theia::runtime::TipcMux mux;
    DiagRef                 ref;
    bool                    started = false;
    std::mutex              call_mu;
};

DiagLink::DiagLink() : impl_(new Impl()) {}
DiagLink::~DiagLink() { stop(); delete impl_; }

DiagLink& DiagLink::instance() {
    static DiagLink s;
    return s;
}

bool DiagLink::start(int connect_timeout_ms) {
    if (impl_->started) return true;
    if (!impl_->ref.connect(connect_timeout_ms)) return false;
    impl_->mux.watch_remote_ref(impl_->ref);
    impl_->mux.start();
    impl_->started = true;
    return true;
}

void DiagLink::stop() {
    if (!impl_->started) return;
    impl_->mux.stop();
    impl_->started = false;
}

bool DiagLink::connected() const { return impl_->started; }

DiagUdsResult DiagLink::send_uds(uint32_t target_addr, const std::string& uds,
                                 int timeout_ms) {
    DiagUdsResult out;
    if (!impl_->started) return out;
    std::lock_guard<std::mutex> lk(impl_->call_mu);

    system_services_diag_UdsRequest req = system_services_diag_UdsRequest_init_zero;
    req.source_addr = 0x0E80;        // a conventional tester address
    req.target_addr = target_addr;
    const size_t n = std::min(uds.size(), sizeof(req.uds.bytes));
    std::memcpy(req.uds.bytes, uds.data(), n);
    req.uds.size = static_cast<pb_size_t>(n);

    auto result = theia::runtime::call<system_services_diag_UdsReply>(
        impl_->ref, req, /*act=*/0, timeout_ms);
    if (result.tag != theia::runtime::CallTag::Reply) return out;   // ok=false

    const auto& rep = result.reply;
    out.uds.assign(reinterpret_cast<const char*>(rep.uds.bytes), rep.uds.size);
    out.is_nrc = rep.is_nrc;
    out.ok = true;
    return out;
}

}  // namespace services_com

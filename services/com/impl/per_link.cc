// per_link implementation — RemoteRef + reply-pump TipcMux to PerManager.
//
// The nanopb per structs live ONLY here (per_codecs.hh + the per pb headers),
// confined to this TU so the libprotobuf gRPC edge in ComGrpcProxy_handlers.cc
// stays free of them. Mirrors sup_link.cc.

#include "impl/per_link.hpp"

#include "NodeRef.hh"
#include "TipcMux.hh"
#include "system/services/per/per.pb.h"   // nanopb per structs
#include "lib/per_codecs.hh"              // RemoteCodec specializations

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace services_com {

namespace {

// PerManager (services/per) — schema registry + snapshot ops. Moved to
// 0x80010016 off the old 0x80010008, which collided with com's ComDaemon.
constexpr uint32_t kPerMgrTipcType     = 0x80010016u;
constexpr uint32_t kPerMgrTipcInstance = 0u;

struct PerManagerTag {
    static constexpr const char* kNodeName = "per_manager";
};
using PerRef =
    theia::runtime::RemoteRef<PerManagerTag, kPerMgrTipcType, kPerMgrTipcInstance>;

void set_str(char* dst, size_t cap, const std::string& s) {
    std::snprintf(dst, cap, "%s", s.c_str());
}

std::string from_str(const char* s) { return std::string(s ? s : ""); }

}  // namespace

struct PerLink::Impl {
    theia::runtime::TipcMux mux;     // reply pump for the ref's client fd
    PerRef                  ref;
    bool                    started = false;
    std::mutex              call_mu;
};

PerLink::PerLink() : impl_(new Impl()) {}
PerLink::~PerLink() { stop(); delete impl_; }

PerLink& PerLink::instance() {
    static PerLink s;
    return s;
}

bool PerLink::start(int connect_timeout_ms) {
    if (impl_->started) return true;
    if (!impl_->ref.connect(connect_timeout_ms)) return false;
    impl_->mux.watch_remote_ref(impl_->ref);
    impl_->mux.start();
    impl_->started = true;
    return true;
}

void PerLink::stop() {
    if (!impl_->started) return;
    impl_->mux.stop();
    impl_->started = false;
}

bool PerLink::connected() const { return impl_->started; }

bool PerLink::list_schemas(const std::string& config_type,
                           std::vector<PerSchema>& out, int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    system_services_per_ListSchemasReq req =
        system_services_per_ListSchemasReq_init_zero;
    set_str(req.config_type, sizeof(req.config_type), config_type);

    auto result = theia::runtime::call<system_services_per_SchemaList>(
        impl_->ref, req, /*act=*/0, timeout_ms);
    if (result.tag != theia::runtime::CallTag::Reply) return false;

    const auto& list = result.reply;
    out.clear();
    for (pb_size_t i = 0; i < list.schemas_count; ++i) {
        PerSchema s;
        s.config_type = from_str(list.schemas[i].config_type);
        s.digest      = from_str(list.schemas[i].digest);
        out.push_back(std::move(s));
    }
    return true;
}

bool PerLink::snapshot(const std::string& label, PerOpReply& out,
                       int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    system_services_per_SnapshotReq req =
        system_services_per_SnapshotReq_init_zero;
    set_str(req.label, sizeof(req.label), label);

    auto result = theia::runtime::call<system_services_per_PerReply>(
        impl_->ref, req, /*act=*/0, timeout_ms);
    if (result.tag != theia::runtime::CallTag::Reply) return false;
    out.status  = result.reply.status;
    out.message = from_str(result.reply.message);
    out.mod_rev = result.reply.mod_rev;
    return true;
}

}  // namespace services_com

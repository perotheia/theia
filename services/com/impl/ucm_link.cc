// ucm_link implementation — RemoteRef call to UcmDaemon (RequestUpdate) + a
// subscriber to UcmFsm's UcmProgressStream (the latest FSM state). The nanopb ucm
// structs live ONLY here, confined to this TU so the libprotobuf gRPC edge in
// ComGrpcProxy_handlers.cc stays free of them. Mirrors per_link.cc / shwa_link.cc.

#include "impl/ucm_link.hpp"

#include "NodeRef.hh"
#include "TipcMux.hh"
#include "PgClient.hh"                     // PG group join (UcmProgress multicast)
#include "system/services/ucm/ucm.pb.h"   // nanopb ucm structs
#include "lib/ucm_codecs.hh"              // RemoteCodec<PackageManifest/UcmReply/UcmProgress>
#include <pb_decode.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace services_com {

namespace {

// UcmDaemon (services/ucm) — the ara::ucm agent ctl front, UpdateCtl.RequestUpdate.
constexpr uint32_t kUcmDaemonTipcType     = 0x8001000Eu;
constexpr uint32_t kUcmDaemonTipcInstance = 0u;
// UcmFsm broadcasts UcmProgress on a PG group keyed by the proto type name
// (msg_type_name<UcmProgress>() == the defining package). com pg_joins it to fold
// the latest sample, exactly like shwa_link folds AccelSample.
constexpr const char* kUcmProgressGroup = "system_services_ucm_UcmProgress";

struct UcmDaemonTag { static constexpr const char* kNodeName = "ucm_daemon"; };
using UcmRef =
    theia::runtime::RemoteRef<UcmDaemonTag, kUcmDaemonTipcType, kUcmDaemonTipcInstance>;

void set_str(char* dst, size_t cap, const std::string& s) {
    std::snprintf(dst, cap, "%s", s.c_str());
}
std::string from_str(const char* s) { return std::string(s ? s : ""); }

}  // namespace

struct UcmLink::Impl {
    theia::runtime::TipcMux mux;     // reply pump for the ref's client fd (RequestUpdate)
    UcmRef                  ref;
    ::theia::runtime::PgClient pg;   // UcmProgress group membership
    bool                    started = false;
    std::mutex              call_mu;

    std::mutex              prog_mu;
    UcmProgressSample       latest;  // last UcmProgress seen on the group
};

UcmLink::UcmLink() : impl_(new Impl()) {}
UcmLink::~UcmLink() { stop(); delete impl_; }

UcmLink& UcmLink::instance() {
    static UcmLink s;
    return s;
}

bool UcmLink::start(int connect_timeout_ms) {
    if (impl_->started) return true;
    if (!impl_->ref.connect(connect_timeout_ms)) return false;
    impl_->mux.watch_remote_ref(impl_->ref);
    impl_->mux.start();

    // pg_join the UcmProgress group: each multicast UcmProgress lands here as raw
    // bytes; decode it (nanopb) and fold into `latest` (the GS poll reads it).
    // Mirrors shwa_link's AccelSample fold. A failed join (supervisor down) just
    // leaves latest.valid=false — RequestUpdate still works.
    impl_->pg.attach("com-ucm-link", /*binding=*/nullptr);
    auto g = impl_->pg.join_raw_named(kUcmProgressGroup,
        [this](const uint8_t* p, uint16_t len) {
            system_services_ucm_UcmProgress prog =
                system_services_ucm_UcmProgress_init_zero;
            pb_istream_t is = pb_istream_from_buffer(p, len);
            if (!pb_decode(&is,
                    theia::runtime::RemoteCodec<system_services_ucm_UcmProgress>::fields(),
                    &prog))
                return;
            std::lock_guard<std::mutex> lk(impl_->prog_mu);
            impl_->latest.state   = static_cast<uint32_t>(prog.state);
            impl_->latest.version = from_str(prog.version);
            impl_->latest.kind    = static_cast<uint32_t>(prog.kind);
            impl_->latest.scope   = static_cast<uint32_t>(prog.scope);
            impl_->latest.detail  = from_str(prog.detail);
            impl_->latest.ts_ns   = prog.ts_ns;
            impl_->latest.valid   = true;
        });
    if (!g.ok)
        std::fprintf(stderr,
            "[ucm_link] pg_join of the UcmProgress group failed "
            "(supervisor down?); progress unavailable\n");

    impl_->started = true;
    return true;
}

void UcmLink::stop() {
    if (!impl_->started) return;
    impl_->pg.shutdown();
    impl_->mux.stop();
    impl_->started = false;
}

bool UcmLink::connected() const { return impl_->started; }

bool UcmLink::request_update(const UcmUpdateReq& req, uint32_t& status_out,
                             int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);

    system_services_ucm_PackageManifest m =
        system_services_ucm_PackageManifest_init_zero;
    set_str(m.name, sizeof(m.name), req.name);
    set_str(m.version, sizeof(m.version), req.version);
    m.kind  = static_cast<system_services_ucm_UpdateKind>(req.kind);
    m.scope = static_cast<system_services_ucm_UpdateScope>(req.scope);
    set_str(m.artifact_path, sizeof(m.artifact_path), req.artifact_path);
    set_str(m.signature, sizeof(m.signature), req.signature);

    auto result = theia::runtime::call<system_services_ucm_UcmReply>(
        impl_->ref, m, /*act=*/0, timeout_ms);
    if (result.tag != theia::runtime::CallTag::Reply) return false;
    status_out = result.reply.status;
    return true;
}

UcmProgressSample UcmLink::latest_progress() {
    std::lock_guard<std::mutex> lk(impl_->prog_mu);
    return impl_->latest;
}

}  // namespace services_com

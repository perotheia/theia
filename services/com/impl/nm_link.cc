// nm_link implementation — RemoteRef + reply-pump TipcMux to NmDaemon.
//
// The nanopb nm structs live ONLY here (nm_codecs.hh + nm.pb.h), confined to
// this TU so the libprotobuf gRPC edge in ComGrpcProxy_handlers.cc stays free of
// them. Mirrors per_link.cc.

#include "impl/nm_link.hpp"

#include "NodeRef.hh"
#include "TipcMux.hh"
#include "system/services/nm/nm.pb.h"   // nanopb nm structs
#include "lib/nm_codecs.hh"             // RemoteCodec specializations

#include <cstdio>
#include <cstring>
#include <mutex>

namespace services_com {

namespace {

// NmDaemon (services/nm) — the readiness FSM + wifi observation surface.
constexpr uint32_t kNmTipcType     = 0x8001002Eu;
constexpr uint32_t kNmTipcInstance = 0u;

struct NmDaemonTag {
    static constexpr const char* kNodeName = "nm_daemon";
};
using NmRef =
    theia::runtime::RemoteRef<NmDaemonTag, kNmTipcType, kNmTipcInstance>;

void set_str(char* dst, size_t cap, const std::string& s) {
    std::snprintf(dst, cap, "%s", s.c_str());
}
std::string from_str(const char* s) { return std::string(s ? s : ""); }

}  // namespace

struct NmLink::Impl {
    theia::runtime::TipcMux mux;
    NmRef                   ref;
    bool                    started = false;
    std::mutex              call_mu;
};

NmLink::NmLink() : impl_(new Impl()) {}
NmLink::~NmLink() { stop(); delete impl_; }

NmLink& NmLink::instance() {
    static NmLink s;
    return s;
}

bool NmLink::start(int connect_timeout_ms) {
    if (impl_->started) return true;
    if (!impl_->ref.connect(connect_timeout_ms)) return false;
    impl_->mux.watch_remote_ref(impl_->ref);
    impl_->mux.start();
    impl_->started = true;
    return true;
}

void NmLink::stop() {
    if (!impl_->started) return;
    impl_->mux.stop();
    impl_->started = false;
}

bool NmLink::connected() const { return impl_->started; }

bool NmLink::get_status(NmStatusInfo& out, int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    system_services_nm_NetStatusReq req = system_services_nm_NetStatusReq_init_zero;

    auto result = theia::runtime::call<system_services_nm_NmStatusMsg>(
        impl_->ref, req, /*act=*/0, timeout_ms);
    if (result.tag != theia::runtime::CallTag::Reply) return false;

    const auto& s = result.reply;
    out.state       = static_cast<uint32_t>(s.state);
    out.interface   = from_str(s.interface);
    out.has_carrier = s.has_carrier;
    out.has_address = s.has_address;
    out.vpn_up      = s.vpn_up;
    out.ts_ns       = s.ts_ns;
    return true;
}

bool NmLink::wifi_scan(const std::string& interface, NmWifiScanInfo& out,
                       int timeout_ms) {
    if (!impl_->started) return false;
    std::lock_guard<std::mutex> lk(impl_->call_mu);
    system_services_nm_WifiScanReq req = system_services_nm_WifiScanReq_init_zero;
    set_str(req.interface, sizeof(req.interface), interface);

    auto result = theia::runtime::call<system_services_nm_WifiScanReply>(
        impl_->ref, req, /*act=*/0, timeout_ms);
    if (result.tag != theia::runtime::CallTag::Reply) return false;

    const auto& r = result.reply;
    out.interface   = from_str(r.interface);
    out.associated  = r.associated;
    out.assoc_ssid  = from_str(r.assoc_ssid);
    out.assoc_bssid = from_str(r.assoc_bssid);
    out.bss.clear();
    for (pb_size_t i = 0; i < r.bss_count; ++i) {
        const auto& b = r.bss[i];
        NmBssInfo row;
        row.ssid       = from_str(b.ssid);
        row.bssid      = from_str(b.bssid);
        row.signal_dbm = b.signal_dbm;
        row.freq_mhz   = b.freq_mhz;
        row.security   = from_str(b.security);
        out.bss.push_back(std::move(row));
    }
    return true;
}

}  // namespace services_com

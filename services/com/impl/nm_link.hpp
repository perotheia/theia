// nm_link — com's RemoteRef link to services/nm's NmDaemon (the readiness FSM +
// wifi observation surface), so com can proxy nm's GetNetworkStatus + WifiScan
// over gRPC (NmView) for the GUI / rtdb. Mirrors per_link: the nanopb nm structs
// + the RemoteRef/TipcMux live ONLY in nm_link.cc, so the libprotobuf gRPC edge
// (ComGrpcProxy_handlers.cc) never meets the nanopb nm headers.
//
// NmDaemon is at TIPC 0x8001002E/0. Read-only: the readiness snapshot + the
// visible-AP scan. The connect path (WifiConnect) is a node↔nm op, not proxied.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace services_com {

// The readiness snapshot, in primitives (mirrors system_services_nm_NmStatusMsg).
struct NmStatusInfo {
    uint32_t    state = 0;          // NetState ordinal
    std::string interface;
    bool        has_carrier = false;
    bool        has_address = false;
    bool        vpn_up = false;
    uint64_t    ts_ns = 0;
};

// One visible AP row (mirrors system_services_nm_WifiBss).
struct NmBssInfo {
    std::string ssid;
    std::string bssid;
    int32_t     signal_dbm = 0;
    uint32_t    freq_mhz = 0;
    std::string security;
};

// A WifiScan result, in primitives (mirrors system_services_nm_WifiScanReply).
struct NmWifiScanInfo {
    std::string interface;
    bool        associated = false;
    std::string assoc_ssid;
    std::string assoc_bssid;
    std::vector<NmBssInfo> bss;
};

// Singleton link to NmDaemon. Opened by ComGrpcProxy::do_start, torn down by
// do_stop. Thread-safe.
class NmLink {
public:
    static NmLink& instance();

    // Connect the RemoteRef (TIPC 0x8001002E/0) + start the reply pump.
    // Returns false if nm isn't reachable. Idempotent.
    bool start(int connect_timeout_ms = 3000);
    void stop();
    bool connected() const;

    // GetNetworkStatus → the readiness snapshot.
    bool get_status(NmStatusInfo& out, int timeout_ms = 5000);
    // WifiScan(interface="" → the monitored wifi link) → APs + association.
    bool wifi_scan(const std::string& interface, NmWifiScanInfo& out,
                   int timeout_ms = 8000);

private:
    NmLink();
    ~NmLink();
    NmLink(const NmLink&) = delete;
    NmLink& operator=(const NmLink&) = delete;
    struct Impl;
    Impl* impl_;
};

}  // namespace services_com

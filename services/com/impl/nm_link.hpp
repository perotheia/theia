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

// NmCfgGate reply: a config-transaction op's outcome. txn_state==2 (PENDING)
// means "applied, awaiting ConfirmConfig over the new path".
struct NmCfgReplyInfo {
    bool        ok = false;
    std::string message;
    uint32_t    profiles = 0;
    uint32_t    txn_state = 0;
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

    // ---- config-transaction ops → NmCfgGate (TIPC 0x8001002F). Each returns
    // the gate's NmCfgReply; false only if the gate is unreachable. ----
    bool add_wifi(const std::string& ssid, const std::string& psk,
                  uint32_t priority, NmCfgReplyInfo& out, int timeout_ms = 4000);
    bool remove_wifi(const std::string& ssid, NmCfgReplyInfo& out,
                     int timeout_ms = 4000);
    bool set_vpn(bool require_vpn, bool auto_vpn, NmCfgReplyInfo& out,
                 int timeout_ms = 4000);
    bool set_autoconnect(bool auto_connect, NmCfgReplyInfo& out,
                         int timeout_ms = 4000);
    bool confirm_config(NmCfgReplyInfo& out, int timeout_ms = 4000);
    bool abort_config(NmCfgReplyInfo& out, int timeout_ms = 4000);

private:
    NmLink();
    ~NmLink();
    NmLink(const NmLink&) = delete;
    NmLink& operator=(const NmLink&) = delete;
    struct Impl;
    Impl* impl_;
};

}  // namespace services_com

// State struct for FwDaemon — APP-OWNED, WRITE-ONCE.
//
// Holds the applied FwConfig + the cached apply result (so GetFirewallStatus
// serves it without re-applying). No kernel state is owned here — nft holds the
// live ruleset; this is the FC's view of its last apply.

#pragma once

#include <cstdint>
#include <string>

#include "fw_backend.hpp"   // FState ordinals

namespace ara::fw {

struct FwDaemonState {
    // Applied config (from FwConfig / on_config_update).
    bool        enabled        = true;
    std::string fw_d_dir       = "config/fw.d";
    std::string dmz_tcp_ports  = "7700,7710,7711,2379";
    std::string default_policy = "drop";
    uint32_t    reassert_ms    = 0;
    // Per-FC egress enforcement (cgroup v2 over osi's slice). Empty = off.
    std::string egress_policy;
    std::string egress_slice   = "theia.slice";
    std::string cgroup_root    = "/sys/fs/cgroup";

    // ---- FW.d custom policy (layered onto the comm-matrix baseline) --------
    std::string grpc_client_cidrs;        // "" = DMZ ports global; else saddr-restrict
    std::string vpn_iface;                // "" = no VPN allow; else `iif "<if>" accept`
    std::string forward_policy;           // "" = no forward chain; "drop" = default-drop
    std::string output_policy;            // "" = egress open; "drop" = default-drop output
    bool        log_drops      = false;   // log+count dropped inbound for idsm/phm
    uint32_t    log_drops_rate = 5;       // per-second cap on the FW_DROP log line

    // Last apply result — the snapshot GetFirewallStatus serves.
    int         state          = F_UNKNOWN;
    uint32_t    rule_count     = 0;
    uint32_t    override_count = 0;
    uint32_t    egress_fc_count = 0;
    std::string message        = "not yet applied";

    // PHM health edge-latch: the last health level reported to PHM (-1 = none
    // yet), so apply_now() escalates only on a level CHANGE — not on every reassert
    // tick (the reassert re-applies on a timer; a healthy re-apply must not spam).
    int         last_health    = -1;
};

}  // namespace ara::fw

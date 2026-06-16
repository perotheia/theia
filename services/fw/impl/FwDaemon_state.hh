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

    // Last apply result — the snapshot GetFirewallStatus serves.
    int         state          = F_UNKNOWN;
    uint32_t    rule_count     = 0;
    uint32_t    override_count = 0;
    uint32_t    egress_fc_count = 0;
    std::string message        = "not yet applied";
};

}  // namespace ara::fw

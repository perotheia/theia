// State struct for NmPoller — APP-OWNED, WRITE-ONCE.
//
// The poller's bookkeeping: the LAST link/address observation (so a tick only
// post_event()s the FSM on an EDGE, not every poll), plus the applied config
// (which interface to watch, poll cadence, whether a routable address is
// required for readiness). No network state is owned here — it's a cache of what
// `ip` last reported.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "impl/nm_backend.hpp"   // WifiProfileInfo (the connect-policy profiles)

namespace ara::nm {

struct NmPollerState {
    // Last observation — the edge-detection baseline. `primed` is false until
    // the first poll completes, so the first tick always emits the initial
    // edges (from the FSM's DOWN initial state up to the observed level).
    bool has_carrier = false;
    bool has_address = false;
    bool wifi_assoc  = false;   // last observed wifi-association state (edge base)
    bool vpn_up      = false;   // last observed VPN-rung state (edge base)
    bool primed      = false;

    // Applied config (the poller reads the same etcd NmConfig as NmDaemon via
    // its own ConfigUpdated cast — main.cc registers it on both nodes).
    std::string interfaces;          // "" = auto (first carrier link)
    uint32_t    poll_ms = 1000;
    bool        require_address = true;
    bool        require_vpn = false;     // gate NETWORK_OPERATIONAL on the tunnel
    std::string vpn_interface;           // "" = auto ("tailscale0")

    // Connect policy (NM DRIVES wpa_supplicant when no usable link is up).
    bool        auto_connect = false;
    std::vector<WifiProfileInfo> wifi_profiles;   // known networks, priority-ranked
    // Throttle: only attempt a connect every N ticks so a failing associate
    // doesn't hammer wpa every poll. Counts ticks since the last attempt.
    uint32_t    connect_cooldown = 0;

    // VPN connect policy (NM DRIVES `tailscale up` over the WiFi underlay when
    // auto_vpn + require_vpn and an address is up but the tunnel is down).
    bool        auto_vpn = false;
    std::string vpn_authkey;             // optional key for the unattended `up`
    uint32_t    vpn_cooldown = 0;        // throttle, like connect_cooldown

    // ── Roaming / PHM degradation: count consecutive failed associate/DHCP
    // attempts. After a threshold the poller emits a health event (→ PHM via the
    // supervisor watchdog today) and backs off harder.
    uint32_t    connect_failures = 0;
};

}  // namespace ara::nm

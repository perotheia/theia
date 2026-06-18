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
};

}  // namespace ara::nm

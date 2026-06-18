// User handler bodies for NmPoller — the read-only netlink poller.
//
// HAND-OWNED (gen-app emits this once, then skips it without --force).
//
// NmPoller reads `ip` link/address state on a timer tick (requires_timers), and
// on a STATE EDGE post_event()s into NmDaemon's FSM IN-PROCESS:
//   carrier gained → LinkUp        carrier lost → LinkDown
//   global addr gained → AddrAcquired   global addr lost → AddrLost
// It never configures anything — the kernel + systemd-networkd do. This is the
// driver half of the sm-style statem split (NmDaemon is the FSM; this is its
// gate/event-source).
//
// This TU OWNS the process-global LocalRef<NmDaemon> singleton both nodes share
// (NmDaemon_handlers.cc forward-declares it and publishes *this into it on its
// first state entry). post_event() enqueues onto the FSM's mailbox — the
// thread-safe cross-thread path.

#include "lib/NmPoller.hh"
#include "lib/NmDaemon.hh"
#include "impl/nm_backend.hpp"  // probe_link() — the `ip` observer

#include "TimerService.hh"    // post_info / send_after / process_timers
#include "GenStateM.hh"       // theia::runtime::post_event
#include "NodeRef.hh"         // theia::runtime::LocalRef

#include <pb_decode.h>

#include <cstring>
#include <string>

namespace ara::nm {

// The FSM peer this poller drives. IMPL-OWNED shared singleton, DEFINED here.
// NmDaemon::on_enter publishes itself into it on first entry (during
// start_statem(), before this poller's first tick), so by the time an edge fires
// the ref is valid. NmDaemon_handlers.cc forward-declares this.
theia::runtime::LocalRef<NmDaemon>& nm_statem_ref() {
    static theia::runtime::LocalRef<NmDaemon> ref;
    return ref;
}

namespace {

// Post an FSM event if the statem peer is wired. The first ticks can race the
// FSM's initial-entry publish in pathological scheduling; dropping an edge then
// is harmless — the next tick re-diffs against the same baseline and re-emits.
template <typename Evt>
void post_edge(const char* node, const char* name, Evt evt) {
    auto& ref = nm_statem_ref();
    if (!ref.valid()) {
        std::fprintf(stderr,
            "[%s] %s edge before FSM wired — dropping (will re-emit)\n",
            node, name);
        return;
    }
    theia::runtime::post_event(ref.target(), std::move(evt));
}

}  // namespace

// init: kick off the self-driving poll loop. requires_timers gives us
// send_after; the first "poll" runs immediately and primes the baseline.
void NmPoller::init(NmPollerState& /*s*/) {
    log().info("nm poller up — observing `ip` link/addr (read-only; the kernel "
               "configures the network)");
    ::theia::runtime::post_info(*this, "poll");
}

// handle_info "poll": probe the monitored interface, diff vs the last reading,
// post_event() the carrier/address EDGES into NmDaemon's FSM, then reschedule at
// the config cadence.
void NmPoller::handle_info(const char* info, NmPollerState& s) {
    if (!info || std::strcmp(info, "poll") != 0) return;

    // The FIRST configured interface (NmConfig.interfaces is a comma list).
    std::string want = s.interfaces;
    if (auto comma = want.find(','); comma != std::string::npos)
        want = want.substr(0, comma);

    LinkObservation obs = probe_link(want);
    // require_address=false → treat link-up alone as "address present" so the
    // FSM reaches READY on carrier without waiting for a routable address.
    const bool addr = s.require_address ? obs.has_address : obs.has_carrier;

    // WiFi association: a CHEAP `iw dev <if> link` probe (no radio scan) ONLY for
    // a wireless monitored interface. Wired links report assoc=false and skip the
    // WIFI_ASSOCIATED rung. The full scan is on-demand via the WifiScan op, never
    // per-tick. assoc is only meaningful while carrier is up.
    bool wifi_assoc = false;
    if (obs.has_carrier) {
        WifiObservation w;
        if (wifi_assoc_probe(want, w)) wifi_assoc = w.associated;
    }

    // VPN rung. Only meaningful once an address is up (the tunnel rides on the
    // LAN). When require_vpn=false the rung is SKIPPED — we treat it as trivially
    // satisfied (vpn_up == addr) so IP_ACQUIRED promotes straight to
    // NETWORK_OPERATIONAL. When require_vpn=true we OBSERVE the tunnel
    // (`tailscale status`) and only report up once it's actually established.
    bool vpn_up = false;
    if (addr) {
        if (!s.require_vpn) {
            vpn_up = true;                       // no VPN needed → rung satisfied
        } else {
            VpnObservation v;
            if (vpn_observe(s.vpn_interface, v)) vpn_up = v.up;
        }
    }

    // Edge ORDER matters: the ladder is LINK_AVAILABLE → WIFI_ASSOCIATED →
    // IP_ACQUIRED → VPN_ESTABLISHED, so emit LinkUp → WifiAssociated →
    // AddrAcquired → VpnUp; on teardown reverse (VpnDown first).
    if (!s.primed) {
        // First observation: emit the edges that take the FSM from its
        // NETWORK_OFF initial state up to the observed level.
        s.primed = true;
        if (obs.has_carrier) post_edge(kNodeName, "LinkUp", LinkUp{});
        if (wifi_assoc)      post_edge(kNodeName, "WifiAssociated", WifiAssociated{});
        if (addr)            post_edge(kNodeName, "AddrAcquired", AddrAcquired{});
        if (vpn_up)          post_edge(kNodeName, "VpnUp", VpnUp{});
        log().info(std::string("primed: iface=") +
            (obs.interface.empty() ? "(none)" : obs.interface) +
            " carrier=" + (obs.has_carrier ? "1" : "0") +
            " wifi_assoc=" + (wifi_assoc ? "1" : "0") +
            " addr=" + (addr ? "1" : "0") +
            " vpn=" + (vpn_up ? "1" : "0") +
            (s.require_vpn ? " (vpn required)" : ""));
    } else {
        // Steady state: only emit on a CHANGE. Build-up forward, teardown reverse.
        if (obs.has_carrier != s.has_carrier) {
            if (obs.has_carrier) post_edge(kNodeName, "LinkUp", LinkUp{});
            else                 post_edge(kNodeName, "LinkDown", LinkDown{});
        }
        if (wifi_assoc != s.wifi_assoc) {
            if (wifi_assoc) post_edge(kNodeName, "WifiAssociated", WifiAssociated{});
            else            post_edge(kNodeName, "WifiDisassociated", WifiDisassociated{});
        }
        if (addr != s.has_address) {
            if (addr) post_edge(kNodeName, "AddrAcquired", AddrAcquired{});
            else      post_edge(kNodeName, "AddrLost", AddrLost{});
        }
        if (vpn_up != s.vpn_up) {
            if (vpn_up) post_edge(kNodeName, "VpnUp", VpnUp{});
            else        post_edge(kNodeName, "VpnDown", VpnDown{});
        }
    }
    s.has_carrier = obs.has_carrier;
    s.has_address = addr;
    s.wifi_assoc  = wifi_assoc;
    s.vpn_up      = vpn_up;

    uint32_t ms = s.poll_ms ? s.poll_ms : 1000;
    ::theia::runtime::send_after(::theia::runtime::process_timers(), ms,
                                 *this, "poll");
}

// on_config_update: apply NmConfig live (which interface to watch, poll cadence,
// require_address gate). The next tick uses it; no restart.
void NmPoller::on_config_update(
        const platform_runtime_ConfigUpdated& cfg,
        NmPollerState& s) {
    system_services_nm_NmConfig c = system_services_nm_NmConfig_init_zero;
    pb_istream_t is = pb_istream_from_buffer(
        reinterpret_cast<const pb_byte_t*>(cfg.config.bytes), cfg.config.size);
    if (!pb_decode(&is, system_services_nm_NmConfig_fields, &c)) {
        log().warn("on_config_update: NmConfig decode failed — not applied");
        return;
    }
    s.interfaces      = c.interfaces;
    s.poll_ms         = c.poll_ms ? c.poll_ms : 1000;
    s.require_address = c.require_address;
    s.require_vpn     = c.require_vpn;
    s.vpn_interface   = c.vpn_interface;
    // Re-prime so the next tick re-emits edges against the new interface/gate.
    s.primed = false;
    log().info(std::string("config: interfaces='") + c.interfaces +
        "' poll_ms=" + std::to_string(s.poll_ms) +
        " require_address=" + (s.require_address ? "true" : "false") +
        " require_vpn=" + (s.require_vpn ? "true" : "false") +
        " vpn_iface='" + s.vpn_interface + "'");
}

}  // namespace ara::nm

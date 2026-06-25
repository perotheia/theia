// User handler bodies for NmDaemon (STATEM variant) — the network-readiness FSM.
//
// HAND-OWNED (gen-app emits this once, then skips it without --force).
//
// NmDaemon is the readiness state machine. It takes NO external TIPC events: the
// sibling NmPoller (same process) reads `ip` link/addr state on a tick and
// post_event()s LinkUp/LinkDown/AddrAcquired/AddrLost into this FSM IN-PROCESS
// via a process-global LocalRef<NmDaemon>, which NmDaemon publishes here on its
// first state entry. This mirrors sm's SmDaemon/SmGate split.
//
// What this file owns:
//   - on_enter — publish self to the poller's ref (once), stamp the readiness
//     snapshot (state + ts_ns) into the FSM data, and BROADCAST it to every
//     NmStatusStream subscriber (SM gates "operational" on READY).
//   - handle_call(NetStatusReq) — serve GetNetworkStatus from the FSM data.
//   - on_config_update — keep the advertised interface name in step with config.

#include "lib/NmDaemon.hh"
#include "impl/nm_backend.hpp"   // wifi_observe() — read-only `iw` scan/assoc

#include "NodeRef.hh"     // theia::runtime::LocalRef — publish self to the poller
#include "GenStateM.hh"   // theia::runtime::GenStateMHolder
#include "ParamsConfig.hh"  // get_config() — the rig's monitored-iface prop

#include <pb_decode.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace ara::nm {

// The FSM peer NmPoller post_event()s into. DEFINED in NmPoller_handlers.cc
// (impl-owned shared singleton); on_enter publishes `*this` into it on the FIRST
// state entry (which happens during start_statem(), before the poller's first
// tick).
theia::runtime::LocalRef<NmDaemon>& nm_statem_ref();

namespace {

uint64_t now_ns_() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

// Map the C++ FSM state (its own DENSE enum, in `states [...]` order:
// NETWORK_OFF=0, LINK_AVAILABLE=1, WIFI_ASSOCIATED=2, IP_ACQUIRED=3,
// VPN_ESTABLISHED=4, NETWORK_OPERATIONAL=5, DEGRADED=6) to the WIRE NetState
// (NETWORK_OFF=0, LINK_AVAILABLE=1, WIFI_ASSOCIATED=4, IP_ACQUIRED=2,
// VPN_ESTABLISHED=5, NETWORK_OPERATIONAL=6, DEGRADED=3). The two enums DON'T
// share values — never cast one to the other; switch explicitly.
system_services_nm_NetState wire_state_(NmDaemonState s) {
    // nanopb names enum constants <pkg>_<EnumName>_<ValueName>; the proto enum is
    // `NetState` with values `NetState_*`, so the C constant doubles the name:
    // system_services_nm_NetState_NetState_NETWORK_OFF, etc.
    switch (s) {
    case NmDaemonState::NETWORK_OFF:         return system_services_nm_NetState_NetState_NETWORK_OFF;
    case NmDaemonState::LINK_AVAILABLE:      return system_services_nm_NetState_NetState_LINK_AVAILABLE;
    case NmDaemonState::WIFI_ASSOCIATED:     return system_services_nm_NetState_NetState_WIFI_ASSOCIATED;
    case NmDaemonState::IP_ACQUIRED:         return system_services_nm_NetState_NetState_IP_ACQUIRED;
    case NmDaemonState::VPN_ESTABLISHED:     return system_services_nm_NetState_NetState_VPN_ESTABLISHED;
    case NmDaemonState::NETWORK_OPERATIONAL: return system_services_nm_NetState_NetState_NETWORK_OPERATIONAL;
    case NmDaemonState::DEGRADED:            return system_services_nm_NetState_NetState_DEGRADED;
    }
    return system_services_nm_NetState_NetState_NETWORK_OFF;
}

// Derive the carrier/address/vpn truth the snapshot advertises from the readiness
// state. The poller drives the EDGES; the FSM state is the authoritative
// readiness level, so the booleans follow it (rather than threading raw poller
// observations through every event payload).
void stamp_snapshot_(NmDaemonState s, NmStatusMsg& d) {
    d.state = wire_state_(s);
    switch (s) {
    case NmDaemonState::NETWORK_OFF:
        d.has_carrier = false; d.has_address = false; d.vpn_up = false; break;
    case NmDaemonState::LINK_AVAILABLE:
        d.has_carrier = true;  d.has_address = false; d.vpn_up = false; break;
    case NmDaemonState::WIFI_ASSOCIATED:
        // Wifi carrier + AP association, awaiting DHCP. Carrier yes, addr not yet.
        d.has_carrier = true;  d.has_address = false; d.vpn_up = false; break;
    case NmDaemonState::IP_ACQUIRED:
        d.has_carrier = true;  d.has_address = true;  d.vpn_up = false; break;
    case NmDaemonState::VPN_ESTABLISHED:
    case NmDaemonState::NETWORK_OPERATIONAL:
        // Tunnel rung reached (or VPN trivially satisfied) — full connectivity.
        d.has_carrier = true;  d.has_address = true;  d.vpn_up = true;  break;
    case NmDaemonState::DEGRADED:
        // Recoverable: a rung was lost. Advertise nothing as guaranteed.
        d.has_carrier = false; d.has_address = false; d.vpn_up = false; break;
    }
    d.ts_ns = now_ns_();
}

}  // namespace

// on_enter — runs on the FSM thread AFTER every committed transition (and once
// at init with new==old==DOWN). SAFE to broadcast/log/post_event; UNSAFE to
// transition. NmDaemonData IS NmStatusMsg (the .art `data NmStatusMsg` alias),
// so mutating `d` updates BOTH the broadcast payload and the FSM's persistent
// data in one move — handle_call(NetStatusReq) reads the same `d` back.
void NmDaemon::on_enter(NmDaemonState new_s,
                          NmDaemonState old_s,
                          NmDaemonData& d) {
    // Publish self to the poller on first entry (idempotent on later
    // transitions). The initial DOWN entry runs during start_statem(), so the
    // ref is wired before NmPoller's first tick could post an edge.
    if (!nm_statem_ref().valid()) {
        nm_statem_ref() = theia::runtime::LocalRef<NmDaemon>(*this);
    }

    stamp_snapshot_(new_s, d);

    // Stamp the monitored-interface label from the RIG PROP (deploy/config/
    // <rig>/nm.json → get_config("nm_poller").interfaces) — the SAME source the
    // poller actually monitors, so `wifi status` reports the real iface, not
    // "(auto)". Only when still empty: a live etcd ConfigUpdated (on_config_update)
    // sets d.interface and must win over the boot prop.
    if (d.interface[0] == '\0') {
        std::string ifc = ::theia::runtime::get_config()
                              .node("nm_poller").str("interfaces", "");
        // NmConfig.interfaces is a comma list; the label shows the first.
        if (auto comma = ifc.find(','); comma != std::string::npos)
            ifc = ifc.substr(0, comma);
        if (!ifc.empty())
            std::strncpy(d.interface, ifc.c_str(), sizeof(d.interface) - 1);
    }

    // state_name() is generated in NmDaemon.hh — single source of truth for the
    // state labels (covers the full ladder, unlike a hand-kept local array).
    this->log().info(std::string("→ ") + NmDaemon::state_name(new_s) +
        " iface=" + (d.interface[0] ? d.interface : "(auto)") +
        " @ " + std::to_string(d.ts_ns));

    // Fan out the readiness snapshot to every NmStatusStream subscriber. SM (and
    // anything gating on network readiness) is a receiver. broadcast_* snapshots
    // subscribers under the lock + invokes outside it, so a slow subscriber
    // can't stall the FSM thread.
    broadcast_broadcast_status(d);

    // PHM health edge (escalation model): report a health INDICATION to PHM only
    // on the OPERATIONAL fault EDGE — NOT every transition (no keep-alive spam;
    // PHM aggregates). NM does its own fault analysis here: reaching OPERATIONAL
    // is the fault CLEARED (OK); leaving OPERATIONAL (or entering DEGRADED) is the
    // fault. PHM aggregates this into the platform health verdict; the orthogonal
    // OPERATIONAL gate (SM FgGate on NmStatusStream) fires on the same edge.
    {
        const bool now_op  = (new_s == NmDaemonState::NETWORK_OPERATIONAL);
        const bool was_op  = (old_s == NmDaemonState::NETWORK_OPERATIONAL);
        if (now_op != was_op) {   // edge only
            FcHealthReport hr = system_services_phm_FcHealthReport_init_zero;
            std::snprintf(hr.entity, sizeof(hr.entity), "%s", NmDaemon::kNodeName);
            hr.fg    = 2;          // FG_NETWORK (sm sm_sup_link FgId)
            hr.ts_ns = d.ts_ns;
            if (now_op) {
                hr.level = system_services_phm_HealthLevel_HealthLevel_OK;
                hr.code  = 0;
                std::snprintf(hr.detail, sizeof(hr.detail), "network operational");
            } else {
                hr.level = system_services_phm_HealthLevel_HealthLevel_DEGRADED;
                hr.code  = 1;      // NM-local: lost operational
                std::snprintf(hr.detail, sizeof(hr.detail),
                              "lost operational (state=%s)", NmDaemon::state_name(new_s));
            }
            broadcast_to_phm_report(hr);
        }
    }

    // VPN_ESTABLISHED self-advances to NETWORK_OPERATIONAL: the tunnel rung is the
    // last gate, and "operational" is the steady terminal state SM gates on. Post
    // the internal Operational event to self (it is NOT in NmEventIn — the poller
    // never emits it). post_event enqueues onto our own mailbox, so this returns
    // immediately and the promotion runs as the next event. Idempotent: from
    // NETWORK_OPERATIONAL there's no Operational transition, so a stray one is a
    // no-op.
    if (new_s == NmDaemonState::VPN_ESTABLISHED) {
        theia::runtime::post_event(*this, Operational{});
    }
}

// GetNetworkStatus — serve the current readiness snapshot straight from the FSM
// data (h.data IS the NmStatusMsg the last on_enter stamped). Read-only.
NmStatusMsg NmDaemon::handle_call(
        const NetStatusReq& /*req*/,
        ::theia::runtime::GenStateMHolder<NmDaemonState, NmDaemonData>& h) {
    return h.data;
}

// WifiScan — observe a wireless interface via `iw` (read-only) and return the
// visible AP list + the link's association snapshot. The request `interface`
// wins; "" falls back to the FSM's monitored interface (h.data.interface), and
// the backend further falls back to the first wireless link. NM never associates
// — iwd/wpa does; this only reports what `iw` shows. Drives tdb/rtdb `wifi`.
WifiScanReply NmDaemon::handle_call(
        const WifiScanReq& req,
        ::theia::runtime::GenStateMHolder<NmDaemonState, NmDaemonData>& h) {
    std::string want = req.interface[0] ? std::string(req.interface)
                                        : std::string(h.data.interface);

    WifiObservation obs = wifi_observe(want);

    WifiScanReply reply = system_services_nm_WifiScanReply_init_zero;
    std::strncpy(reply.interface, obs.interface.c_str(),
                 sizeof(reply.interface) - 1);
    reply.associated = obs.associated;
    std::strncpy(reply.assoc_ssid, obs.assoc_ssid.c_str(),
                 sizeof(reply.assoc_ssid) - 1);
    std::strncpy(reply.assoc_bssid, obs.assoc_bssid.c_str(),
                 sizeof(reply.assoc_bssid) - 1);

    // Pack up to the wire cap (WifiScanReply.bss max_count). Drop the overflow
    // rather than risk an oversized reply (TipcMux 48 KiB cap).
    const size_t cap = sizeof(reply.bss) / sizeof(reply.bss[0]);
    reply.bss_count = 0;
    for (const auto& b : obs.bss) {
        if (reply.bss_count >= cap) break;
        auto& row = reply.bss[reply.bss_count++];
        row = system_services_nm_WifiBss_init_zero;
        std::strncpy(row.ssid, b.ssid.c_str(), sizeof(row.ssid) - 1);
        std::strncpy(row.bssid, b.bssid.c_str(), sizeof(row.bssid) - 1);
        row.signal_dbm = b.signal_dbm;
        row.freq_mhz   = b.freq_mhz;
        std::strncpy(row.security, b.security.c_str(), sizeof(row.security) - 1);
    }

    this->log().info(std::string("WifiScan iface=") +
        (obs.interface.empty() ? "(none)" : obs.interface) +
        " associated=" + (obs.associated ? "1" : "0") +
        " ssid=" + (obs.assoc_ssid.empty() ? "-" : obs.assoc_ssid) +
        " aps=" + std::to_string(reply.bss_count));
    return reply;
}

// WifiConnect — DRIVE wpa_supplicant to associate to an SSID (the connect path).
// NM issues the wpa_cli add/set/enable/select sequence + a DHCP lease; the FSM
// then observes the resulting assoc/addr edges and climbs the ladder. The request
// `interface` wins; "" → the monitored wifi link / first wireless. This is the
// manual entry point (tdb/rtdb `wifi connect`); the auto_connect POLICY in
// NmPoller calls the same backend. NM never performs 802.11 — wpa does.
WifiConnectReply NmDaemon::handle_call(
        const WifiConnectReq& req,
        ::theia::runtime::GenStateMHolder<NmDaemonState, NmDaemonData>& h) {
    std::string want = req.interface[0] ? std::string(req.interface)
                                        : std::string(h.data.interface);
    std::string iface = wifi_iface(want);

    ConnectResult cr = wifi_connect(iface, req.ssid, req.psk);

    WifiConnectReply reply = system_services_nm_WifiConnectReply_init_zero;
    reply.ok = cr.ok;
    std::strncpy(reply.interface, iface.c_str(), sizeof(reply.interface) - 1);
    std::strncpy(reply.message, cr.note.c_str(), sizeof(reply.message) - 1);
    this->log().info(std::string("WifiConnect iface=") +
        (iface.empty() ? "(none)" : iface) + " ssid='" + std::string(req.ssid) +
        "' ok=" + (cr.ok ? "1" : "0") + " — " + cr.note);
    return reply;
}

// on_config_update — keep the FSM's advertised interface NAME in step with
// config so the status snapshot + log reflect the monitored link. The cadence +
// require_address gate are honored poller-side (NmPoller reads the same config).
void NmDaemon::on_config_update(
        const platform_runtime_ConfigUpdated& cfg,
        ::theia::runtime::GenStateMHolder<NmDaemonState, NmDaemonData>& h) {
    system_services_nm_NmConfig c = system_services_nm_NmConfig_init_zero;
    pb_istream_t is = pb_istream_from_buffer(
        reinterpret_cast<const pb_byte_t*>(cfg.config.bytes), cfg.config.size);
    if (!pb_decode(&is, system_services_nm_NmConfig_fields, &c)) {
        this->log().warn("on_config_update: NmConfig decode failed — ignored");
        return;
    }
    std::strncpy(h.data.interface, c.interfaces, sizeof(h.data.interface) - 1);
    h.data.interface[sizeof(h.data.interface) - 1] = '\0';
    this->log().info(std::string("config: interfaces='") + c.interfaces +
        "' poll_ms=" + std::to_string(c.poll_ms) +
        " require_address=" + (c.require_address ? "true" : "false") +
        " require_vpn=" + (c.require_vpn ? "true" : "false"));
}

}  // namespace ara::nm

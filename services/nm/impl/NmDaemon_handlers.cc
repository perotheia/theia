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

#include <pb_decode.h>

#include <chrono>
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

// Map the C++ FSM state (its own DENSE enum: DOWN=0,LINK_UP=1,WIFI_ASSOCIATED=2,
// READY=3,DEGRADED=4) to the WIRE NetState (DOWN=0,LINK_UP=1,WIFI_ASSOCIATED=4,
// READY=2,DEGRADED=3). The two enums DON'T share values — never cast one to the
// other; switch explicitly.
system_services_nm_NetState wire_state_(NmDaemonState s) {
    // nanopb names enum constants <pkg>_<EnumName>_<ValueName>; the proto enum is
    // `NetState` with values `NetState_*`, so the C constant doubles the name:
    // system_services_nm_NetState_NetState_DOWN, etc.
    switch (s) {
    case NmDaemonState::DOWN:            return system_services_nm_NetState_NetState_DOWN;
    case NmDaemonState::LINK_UP:         return system_services_nm_NetState_NetState_LINK_UP;
    case NmDaemonState::WIFI_ASSOCIATED: return system_services_nm_NetState_NetState_WIFI_ASSOCIATED;
    case NmDaemonState::READY:           return system_services_nm_NetState_NetState_READY;
    case NmDaemonState::DEGRADED:        return system_services_nm_NetState_NetState_DEGRADED;
    }
    return system_services_nm_NetState_NetState_DOWN;
}

// Derive the carrier/address truth the snapshot advertises from the readiness
// state. The poller drives the EDGES; the FSM state is the authoritative
// readiness level, so the booleans follow it (rather than threading raw poller
// observations through every event payload).
void stamp_snapshot_(NmDaemonState s, NmStatusMsg& d) {
    d.state = wire_state_(s);
    switch (s) {
    case NmDaemonState::DOWN:
        d.has_carrier = false; d.has_address = false; break;
    case NmDaemonState::LINK_UP:
        d.has_carrier = true;  d.has_address = false; break;
    case NmDaemonState::WIFI_ASSOCIATED:
        // Wifi carrier + AP association, awaiting DHCP. Carrier yes, addr not yet.
        d.has_carrier = true;  d.has_address = false; break;
    case NmDaemonState::READY:
        d.has_carrier = true;  d.has_address = true;  break;
    case NmDaemonState::DEGRADED:
        // Recoverable: link or address was lost. Advertise neither as guaranteed.
        d.has_carrier = false; d.has_address = false; break;
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
                          NmDaemonState /*old_s*/,
                          NmDaemonData& d) {
    // Publish self to the poller on first entry (idempotent on later
    // transitions). The initial DOWN entry runs during start_statem(), so the
    // ref is wired before NmPoller's first tick could post an edge.
    if (!nm_statem_ref().valid()) {
        nm_statem_ref() = theia::runtime::LocalRef<NmDaemon>(*this);
    }

    stamp_snapshot_(new_s, d);

    // state_name() is generated in NmDaemon.hh — single source of truth for the
    // state labels (covers WIFI_ASSOCIATED, unlike a hand-kept local array).
    this->log().info(std::string("→ ") + NmDaemon::state_name(new_s) +
        " iface=" + (d.interface[0] ? d.interface : "(auto)") +
        " @ " + std::to_string(d.ts_ns));

    // Fan out the readiness snapshot to every NmStatusStream subscriber. SM (and
    // anything gating on network readiness) is a receiver. broadcast_* snapshots
    // subscribers under the lock + invokes outside it, so a slow subscriber
    // can't stall the FSM thread.
    broadcast_broadcast_status(d);
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
        " require_address=" + (c.require_address ? "true" : "false"));
}

}  // namespace ara::nm

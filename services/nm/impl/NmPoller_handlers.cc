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
#include "ParamsConfig.hh"    // get_config() — static boot params (deploy override)
#include "impl/nm_cfg_shared.hpp"  // nm_cfg_shared() — live config from the gate/txn

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

// init: seed the BOOT config from static params (deploy/config/<machine>/nm.json,
// overridable per-rig at install), then kick off the self-driving poll loop.
// These are the DEFAULTS before any etcd ConfigUpdated arrives — interfaces /
// auto_connect / auto_vpn / require_vpn are rig-deploy knobs, NOT baked in code:
// the .art default is interfaces="" (auto), and a rig like the rpi4 GPS-feed box
// sets interfaces="wlan0" + auto_connect/auto_vpn in its deploy/config/rpi4/
// nm.json. etcd's ConfigUpdated (on_config_update) can still change them live.
void NmPoller::init(NmPollerState& s) {
    auto cfg = ::theia::runtime::get_config().node(NmPoller::kNodeName);
    s.interfaces      = cfg.str("interfaces", s.interfaces);
    s.poll_ms         = cfg.u32("poll_ms", s.poll_ms);
    s.require_address = cfg.boolean("require_address", s.require_address);
    s.require_vpn     = cfg.boolean("require_vpn", s.require_vpn);
    s.vpn_interface   = cfg.str("vpn_interface", s.vpn_interface);
    s.auto_connect    = cfg.boolean("auto_connect", s.auto_connect);
    s.auto_vpn        = cfg.boolean("auto_vpn", s.auto_vpn);
    s.vpn_authkey     = cfg.str("vpn_authkey", s.vpn_authkey);
    log().info(std::string("nm poller up — boot config: interfaces='") +
               s.interfaces + "' auto_connect=" + (s.auto_connect ? "1" : "0") +
               " auto_vpn=" + (s.auto_vpn ? "1" : "0") +
               " require_vpn=" + (s.require_vpn ? "1" : "0") +
               " (static params; etcd may override live)");
    ::theia::runtime::post_info(*this, "poll");
}

// handle_info "poll": probe the monitored interface, diff vs the last reading,
// post_event() the carrier/address EDGES into NmDaemon's FSM, then reschedule at
// the config cadence.
void NmPoller::handle_info(const char* info, NmPollerState& s) {
    if (!info || std::strcmp(info, "poll") != 0) return;

    // LIVE CONFIG (in-process): the gate/NmCfgTxn keep the effective NmConfig in
    // nm_cfg_shared() — committed normally, `pending` while a txn awaits confirm.
    // Pull it into our state EACH TICK so an `rtdb wifi add`/`vpn on` takes effect
    // immediately, without an etcd→ConfigUpdated round-trip. Only override once
    // the gate has set something (committed_known or a txn is pending); before
    // that, the static-params boot config (init) stands.
    {
        auto& sh = nm_cfg_shared();
        if (sh.committed_known || sh.txn_pending) {
            const auto& c = sh.txn_pending ? sh.pending : sh.committed;
            // MERGE, don't clobber: the gate's NmConfig (from `wifi add`/`vpn on`)
            // carries the wifi_profiles + the flags it set, but NOT the rig's
            // boot-only knobs like `interfaces` (which lives in deploy props, not
            // the runtime config). So preserve the boot `interfaces`/`vpn_interface`
            // when the gate's copy is empty — else an enroll would reset the
            // monitored iface to "" (auto → eth0) and never drive wlan0.
            if (c.interfaces[0]    != '\0') s.interfaces    = c.interfaces;
            if (c.vpn_interface[0] != '\0') s.vpn_interface = c.vpn_interface;
            s.require_vpn  = c.require_vpn;
            s.auto_connect = c.auto_connect;
            s.auto_vpn     = c.auto_vpn;
            s.wifi_profiles.clear();
            for (pb_size_t i = 0; i < c.wifi_profiles_count; ++i) {
                WifiProfileInfo p;
                p.ssid     = c.wifi_profiles[i].ssid;
                p.psk      = c.wifi_profiles[i].psk;
                p.priority = c.wifi_profiles[i].priority;
                s.wifi_profiles.push_back(std::move(p));
            }
        }
    }

    // The FIRST configured interface (NmConfig.interfaces is a comma list).
    std::string want = s.interfaces;
    if (auto comma = want.find(','); comma != std::string::npos)
        want = want.substr(0, comma);

    LinkObservation obs = probe_link(want);
    // require_address=false → treat link-up alone as "address present" so the
    // FSM reaches READY on carrier without waiting for a routable address.
    const bool addr = s.require_address ? obs.has_address : obs.has_carrier;

    // WiFi association: a CHEAP `iw dev <if> link` probe (no radio scan) ONLY when
    // the MONITORED link is itself wireless. A wired monitored link (eth0) skips
    // the WIFI_ASSOCIATED rung entirely — it must NOT report another box's wlan0
    // association. The monitored iface is obs.interface (probe_link resolved the
    // actual link, incl. auto-pick); only treat assoc when THAT link is wireless.
    // The full scan is on-demand via WifiScan, never per-tick.
    bool wifi_assoc = false;
    const std::string mon_iface = obs.interface;   // the link actually monitored
    if (obs.has_carrier && nm_detail::is_wireless(mon_iface)) {
        WifiObservation w;
        if (wifi_assoc_probe(mon_iface, w)) {
            wifi_assoc = w.associated;
            s.assoc_ssid = w.associated ? w.assoc_ssid : std::string();
        }
    }

    // ── CONNECT POLICY (NM drives wpa_supplicant) ──────────────────────────
    // When auto_connect is on and there's NO USABLE link, drive the connection:
    // prefer Ethernet (a wired link with carrier just needs DHCP — nothing to do
    // here), else auto-associate the highest-priority known WiFi profile in range.
    // "No usable link" = no address OR (on a WIRELESS link) not associated — a
    // STALE address left on wlan0 (e.g. a prior manager's lease that wasn't
    // cleared) must NOT block the connect: addr without association isn't usable.
    // Throttled so a failing associate doesn't hammer wpa every tick.
    const bool wifi_unusable = nm_detail::is_wireless(mon_iface) && !wifi_assoc;
    log().info(std::string("connect-gate: auto_connect=") +
        (s.auto_connect ? "1" : "0") + " addr=" + (addr ? "1" : "0") +
        " wifi_unusable=" + (wifi_unusable ? "1" : "0") +
        " profiles=" + std::to_string(s.wifi_profiles.size()) +
        " cooldown=" + std::to_string(s.connect_cooldown) +
        " mon_iface=" + mon_iface);
    if (s.auto_connect && (!addr || wifi_unusable) && !s.wifi_profiles.empty()) {
        if (s.connect_cooldown > 0) {
            --s.connect_cooldown;
        } else if (obs.has_carrier && !nm_detail::is_wireless(want)) {
            // A wired link has carrier but no address yet — leave DHCP to the
            // system; don't drive wifi while ethernet is the live link.
        } else {
            // No usable wired link → pick a wifi profile that's visible and join.
            std::string wif = wifi_iface(want);
            log().info(std::string("connect-try: wif='") + wif + "'");
            if (!wif.empty()) {
                WifiObservation scan = wifi_observe(wif);
                int idx = pick_wifi_profile(s.wifi_profiles, scan.bss);
                log().info(std::string("connect-try: scan aps=") +
                    std::to_string(scan.bss.size()) + " associated=" +
                    (scan.associated ? "1" : "0") + " pick_idx=" +
                    std::to_string(idx));
                if (idx >= 0 && !scan.associated) {
                    const auto& p = s.wifi_profiles[idx];
                    ConnectResult cr = wifi_connect(wif, p.ssid, p.psk);
                    log().info(std::string("auto-connect → '") + p.ssid + "' on " +
                        wif + ": " + (cr.ok ? "ok" : "FAILED") + " (" + cr.note + ")");
                    if (!cr.ok) {
                        if (++s.connect_failures % 5 == 0)
                            log().warn(std::string("wifi auto-connect failing (") +
                                std::to_string(s.connect_failures) +
                                " attempts) — health-degrade signal for PHM");
                    } else {
                        s.connect_failures = 0;
                    }
                    // Back off ~5 ticks to let association + DHCP settle.
                    s.connect_cooldown = 5;
                } else if (idx < 0) {
                    // No known SSID in range — note once per cooldown window.
                    s.connect_cooldown = 5;
                }
            }
        }
    }

    // ── ROAMING POLICY (prefer a higher-priority network that came in range) ──
    // When ALREADY associated, periodically re-scan and, if a known profile with
    // STRICTLY HIGHER priority than the current AP is now visible, switch to it.
    // This is the "walk outside, your phone hotspot appears" case — the connect
    // policy above only fires when there's NO address, so it can't upgrade a live
    // link. Out-of-range is handled by the existing WifiDisassociated→!addr path
    // (re-connect picks the best remaining profile). Slow cadence: a scan is
    // costly + briefly disrupts the link, so don't thrash.
    if (s.auto_connect && wifi_assoc && !s.assoc_ssid.empty() &&
        s.wifi_profiles.size() > 1 && nm_detail::is_wireless(mon_iface)) {
        if (s.roam_cooldown > 0) {
            --s.roam_cooldown;
        } else {
            s.roam_cooldown = 15;   // ~15 ticks between roam scans
            // Priority of the profile we're currently on (−1 if the live AP
            // isn't even one of ours — then any known profile is an upgrade).
            int cur_prio = -1;
            for (const auto& p : s.wifi_profiles)
                if (p.ssid == s.assoc_ssid) { cur_prio = (int)p.priority; break; }
            WifiObservation scan = wifi_observe(mon_iface);
            int best = pick_wifi_profile(s.wifi_profiles, scan.bss);
            if (best >= 0) {
                const auto& bp = s.wifi_profiles[best];
                if (bp.ssid != s.assoc_ssid && (int)bp.priority > cur_prio) {
                    ConnectResult cr = wifi_connect(mon_iface, bp.ssid, bp.psk);
                    log().info(std::string("roam → higher-priority '") + bp.ssid +
                        "' (prio " + std::to_string(bp.priority) + " > " +
                        std::to_string(cur_prio) + ", was '" + s.assoc_ssid +
                        "'): " + (cr.ok ? "switching" : "FAILED") + " (" + cr.note + ")");
                    s.roam_cooldown = 5;   // let the switch + DHCP settle
                }
            }
        }
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

            // CONNECT POLICY (VPN): symmetric to the WiFi drive above. When
            // auto_vpn is on, an address is up (the tunnel rides the LAN) but the
            // tunnel is NOT yet established, DRIVE `tailscale up` pinned to the
            // WiFi underlay. Throttled so a failing `up` doesn't hammer tailscaled
            // every tick. Only over a WIFI link — the whole point is "VPN on wifi".
            const bool on_wifi = nm_detail::is_wireless(obs.interface);
            if (s.auto_vpn && !vpn_up && on_wifi) {
                if (s.vpn_cooldown > 0) {
                    --s.vpn_cooldown;
                } else {
                    ConnectResult vr = vpn_connect(obs.interface, s.vpn_authkey);
                    log().info(std::string("auto-vpn → ") + vr.note);
                    s.vpn_cooldown = 10;   // ~10 ticks for `up` + handshake to settle
                }
            }
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
    s.auto_connect    = c.auto_connect;
    s.auto_vpn        = c.auto_vpn;
    s.vpn_authkey     = c.vpn_authkey;

    // Known WiFi profiles (the connect-policy candidates).
    s.wifi_profiles.clear();
    for (pb_size_t i = 0; i < c.wifi_profiles_count; ++i) {
        WifiProfileInfo p;
        p.ssid     = c.wifi_profiles[i].ssid;
        p.psk      = c.wifi_profiles[i].psk;
        p.priority = c.wifi_profiles[i].priority;
        s.wifi_profiles.push_back(std::move(p));
    }
    s.connect_cooldown = 0;   // allow an immediate connect attempt under the new policy
    s.vpn_cooldown     = 0;

    // Re-prime so the next tick re-emits edges against the new interface/gate.
    s.primed = false;
    log().info(std::string("config: interfaces='") + c.interfaces +
        "' poll_ms=" + std::to_string(s.poll_ms) +
        " require_address=" + (s.require_address ? "true" : "false") +
        " require_vpn=" + (s.require_vpn ? "true" : "false") +
        " auto_connect=" + (s.auto_connect ? "true" : "false") +
        " auto_vpn=" + (s.auto_vpn ? "true" : "false") +
        " profiles=" + std::to_string(s.wifi_profiles.size()) +
        " vpn_iface='" + s.vpn_interface + "'");
}

}  // namespace ara::nm

// nm_backend — the THIN read-only link/address probe behind NmPoller.
//
// APP-OWNED. NM is a control plane, NOT a network configurator: this backend
// only OBSERVES, via raw rtnetlink (RTM_GETLINK + RTM_GETADDR over AF_NETLINK) —
// NOT by shelling `ip`. It reports two booleans for the monitored interface:
// has_carrier (the link's IFF_LOWER_UP flag) and has_address (a routable
// scope-global inet/inet6 address is present). The kernel + systemd-networkd own
// the actual configuration; we never touch it. Uses only present kernel uapi
// headers (linux/rtnetlink.h, if.h, if_addr.h) — no libmnl/libnl dependency.
//
// Interface selection: an explicit name (from NmConfig.interfaces, first of a
// comma list) is honored; "" = auto — the first non-loopback link that has
// carrier (so an unplugged secondary NIC doesn't hold readiness DOWN).
//
// Read-only + best-effort: a netlink socket / send / recv failure degrades to
// "no carrier / no address" (DOWN) rather than throwing.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>
#include <net/if.h>          // IFF_LOOPBACK
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_addr.h>   // ifaddrmsg, IFA_*

// IFF_LOWER_UP (the carrier flag) lives in <linux/if.h>, which clashes with the
// glibc <net/if.h> we already need; define it from its kernel value instead.
#ifndef IFF_LOWER_UP
#define IFF_LOWER_UP 0x10000
#endif

namespace ara::nm {

struct LinkObservation {
    std::string interface;   // the link actually observed ("" if none found)
    bool has_carrier = false;
    bool has_address = false;
};

// One AP from a scan (mirrors the system_services_nm_WifiBss wire row).
struct WifiBssInfo {
    std::string ssid;        // "" = hidden
    std::string bssid;       // aa:bb:cc:dd:ee:ff
    int32_t     signal_dbm = 0;
    uint32_t    freq_mhz   = 0;
    std::string security;    // "open" | "WPA2" | "WPA3" | "WEP"
};

// The result of observing a wireless interface: the latest scan + the link's
// current association snapshot (both from `iw dev <if> ...`).
struct WifiObservation {
    std::string interface;   // the wifi link actually queried ("" if none)
    bool        associated = false;
    std::string assoc_ssid;
    std::string assoc_bssid;
    std::vector<WifiBssInfo> bss;
};

namespace nm_detail {

struct LinkInfo {
    int         index = 0;
    std::string name;
    bool        carrier = false;   // IFF_LOWER_UP
    bool        loopback = false;  // IFF_LOOPBACK
};

// Send one rtnetlink DUMP request (RTM_GETLINK / RTM_GETADDR) and invoke `on_msg`
// for each reply nlmsghdr, parsing EACH recv() batch in place (concatenating
// batches breaks NLMSG_OK alignment — each datagram is self-aligned). Returns
// false on socket error. Stops at NLMSG_DONE.
template <typename Fn>
inline bool nl_dump_(uint16_t type, uint8_t family, Fn on_msg) {
    int fd = ::socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) return false;

    struct { struct nlmsghdr nlh; struct rtgenmsg gen; } req{};
    req.nlh.nlmsg_len   = sizeof(req);
    req.nlh.nlmsg_type  = type;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq   = 1;
    req.gen.rtgen_family = family;
    if (::send(fd, &req, sizeof(req), 0) < 0) { ::close(fd); return false; }

    char buf[16384];
    bool done = false;
    while (!done) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        for (struct nlmsghdr* nlh = (struct nlmsghdr*)buf;
             NLMSG_OK(nlh, (size_t)n); nlh = NLMSG_NEXT(nlh, n)) {
            if (nlh->nlmsg_type == NLMSG_DONE)  { done = true; break; }
            if (nlh->nlmsg_type == NLMSG_ERROR) { done = true; break; }
            on_msg(nlh);
        }
    }
    ::close(fd);
    return true;
}

// Parse RTM_GETLINK dump → the link table (index, name, carrier, loopback).
inline std::vector<LinkInfo> get_links() {
    std::vector<LinkInfo> links;
    nl_dump_(RTM_GETLINK, AF_PACKET, [&](struct nlmsghdr* nlh) {
        if (nlh->nlmsg_type != RTM_NEWLINK) return;
        auto* ifi = (struct ifinfomsg*)NLMSG_DATA(nlh);
        LinkInfo li;
        li.index    = ifi->ifi_index;
        li.carrier  = (ifi->ifi_flags & IFF_LOWER_UP) != 0;
        li.loopback = (ifi->ifi_flags & IFF_LOOPBACK) != 0;
        int alen = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi));
        for (struct rtattr* rta = IFLA_RTA(ifi); RTA_OK(rta, alen);
             rta = RTA_NEXT(rta, alen)) {
            if (rta->rta_type == IFLA_IFNAME)
                li.name = (const char*)RTA_DATA(rta);
        }
        links.push_back(std::move(li));
    });
    return links;
}

// Parse RTM_GETADDR dump → the set of ifindexes that have a routable
// (scope-universe = global) inet/inet6 address.
inline std::set<int> get_global_addr_ifindexes() {
    std::set<int> out;
    nl_dump_(RTM_GETADDR, AF_UNSPEC, [&](struct nlmsghdr* nlh) {
        if (nlh->nlmsg_type != RTM_NEWADDR) return;
        auto* ifa = (struct ifaddrmsg*)NLMSG_DATA(nlh);
        if (ifa->ifa_scope == RT_SCOPE_UNIVERSE)   // global / routable
            out.insert((int)ifa->ifa_index);
    });
    return out;
}

}  // namespace nm_detail

// Probe the monitored interface via rtnetlink. `want` is the desired interface
// name ("" = auto-pick the first non-loopback link with carrier). The poller
// gates has_address into readiness; we report what the kernel shows.
inline LinkObservation probe_link(const std::string& want) {
    using namespace nm_detail;
    LinkObservation obs;

    auto links = get_links();
    if (links.empty()) return obs;             // no netlink / no links → DOWN
    auto global = get_global_addr_ifindexes();

    for (const auto& li : links) {
        if (li.loopback) continue;             // never the loopback

        if (!want.empty()) {
            if (li.name != want) continue;     // explicit: only it
            obs.interface = li.name;
            obs.has_carrier = li.carrier;
            obs.has_address = li.carrier && global.count(li.index) > 0;
            return obs;
        }
        // Auto mode: first non-lo link WITH carrier wins.
        if (li.carrier) {
            obs.interface = li.name;
            obs.has_carrier = true;
            obs.has_address = global.count(li.index) > 0;
            return obs;
        }
        // Remember the first non-lo link even if down, so we report a name
        // (DOWN) rather than "" when nothing has carrier.
        if (obs.interface.empty()) obs.interface = li.name;
    }
    return obs;   // explicit name not found, or no carrier anywhere → as-built
}

// ─── WiFi observation (read-only, via `iw`) ────────────────────────────────
//
// NM does NOT speak 802.11. For scanning + association status it asks `iw`
// (the doc's modern stack is iwd for association; `iw` is the read path). This
// is the ONE place the FC shells out — nl80211 scan-trigger is a CAP_NET_ADMIN
// genetlink state machine, far heavier than the read-only `ip`/netlink probe
// above; the user picked `iw` for the scan. Best-effort: any failure (no iw, no
// wireless iface, permission) degrades to an empty/disassociated observation
// rather than throwing. `iw dev <if> scan` itself needs CAP_NET_ADMIN; without
// it we fall back to `iw dev <if> scan dump` (the cached last scan, no trigger).

namespace nm_detail {

// Run a command, capture stdout. Returns false if popen fails or the command
// exits non-zero (the caller treats that as "no data").
inline bool run_capture(const std::string& cmd, std::string& out) {
    out.clear();
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(
        ::popen((cmd + " 2>/dev/null").c_str(), "r"), ::pclose);
    if (!pipe) return false;
    char buf[4096];
    size_t n;
    while ((n = ::fread(buf, 1, sizeof(buf), pipe.get())) > 0)
        out.append(buf, n);
    // pclose runs at unique_ptr destruction; we can't read its status through
    // the deleter, so success = "we got some bytes". Empty out → treated as
    // no-data by the parsers below, which is the right degrade anyway.
    return true;
}

inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// `iw` prints non-ASCII / non-printable SSID octets as C-style escapes in its
// scan/link output: `\xNN` (a byte) and `\\` (a literal backslash). Un-escape
// them back to the RAW bytes so the SSID is the real UTF-8 string — both for
// display (rtdb shows "Administrator’s iPhone", not "...\xe2\x80\x99s...") AND
// for the CONNECT path (wpa_cli must get the actual SSID octets to match the
// AP). Unknown escapes pass through unchanged.
inline std::string unescape_iw(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            if (n == '\\') { out.push_back('\\'); i += 1; continue; }
            if (n == 'x' && i + 3 < s.size()) {
                auto hex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                int hi = hex(s[i + 2]), lo = hex(s[i + 3]);
                if (hi >= 0 && lo >= 0) {
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 3;
                    continue;
                }
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

// Is `name` a wireless interface? /sys/class/net/<name>/wireless exists for
// 802.11 devices. Avoids running `iw` against eth0.
inline bool is_wireless(const std::string& name) {
    if (name.empty()) return false;
    std::string p = "/sys/class/net/" + name + "/wireless";
    return ::access(p.c_str(), F_OK) == 0;
}

// First wireless interface (auto mode) — scan /sys/class/net for one with a
// wireless/ subdir. "" if none.
inline std::string first_wireless() {
    for (const auto& li : get_links()) {
        if (li.loopback) continue;
        if (is_wireless(li.name)) return li.name;
    }
    return "";
}

// Parse `iw dev <if> scan [dump]` output into BSS rows. The format is one
// "BSS aa:bb:.. (on wlanX)" block per AP, with indented "SSID:", "signal:",
// "freq:", and RSN/WPA capability lines.
inline std::vector<WifiBssInfo> parse_scan(const std::string& text) {
    std::vector<WifiBssInfo> out;
    std::istringstream in(text);
    std::string line;
    WifiBssInfo cur;
    bool have = false;
    bool saw_rsn = false, saw_wpa = false, saw_sae = false, saw_privacy = false;

    auto flush = [&]() {
        if (!have) return;
        // Security classification from the capability/IE lines seen in-block.
        if (saw_sae)            cur.security = "WPA3";
        else if (saw_rsn)       cur.security = "WPA2";
        else if (saw_wpa)       cur.security = "WPA";
        else if (saw_privacy)   cur.security = "WEP";
        else                    cur.security = "open";
        out.push_back(cur);
    };

    while (std::getline(in, line)) {
        std::string t = trim(line);
        // A new AP block starts with a NON-indented "BSS <mac>(on <if>)" line.
        // Indented capability lines like "\tBSS Load:" trim to "BSS Load:" — so
        // gate on the original line being column-0 AND the token after "BSS "
        // being a MAC ("(on " present), not a capability label.
        const bool is_bss_header =
            line.rfind("BSS ", 0) == 0 &&          // column 0 (raw, not trimmed)
            t.find("(on ") != std::string::npos;   // "...(on wlanX)"
        if (is_bss_header) {
            flush();
            cur = WifiBssInfo{};
            have = true;
            saw_rsn = saw_wpa = saw_sae = saw_privacy = false;
            // "BSS aa:bb:cc:dd:ee:ff(on wlan0) -- associated" — take the MAC.
            std::string rest = t.substr(4);
            cur.bssid = trim(rest.substr(0, rest.find('(')));
        } else if (t.rfind("SSID:", 0) == 0) {
            cur.ssid = unescape_iw(trim(t.substr(5)));   // \xNN → real bytes
        } else if (t.rfind("signal:", 0) == 0) {
            // "signal: -54.00 dBm"
            cur.signal_dbm = (int32_t)std::strtol(trim(t.substr(7)).c_str(),
                                                  nullptr, 10);
        } else if (t.rfind("freq:", 0) == 0) {
            cur.freq_mhz = (uint32_t)std::strtoul(trim(t.substr(5)).c_str(),
                                                  nullptr, 10);
        } else if (t.find("RSN:") != std::string::npos) {
            saw_rsn = true;
        } else if (t.find("WPA:") != std::string::npos) {
            saw_wpa = true;
        } else if (t.find("SAE") != std::string::npos) {
            saw_sae = true;   // WPA3-Personal authentication
        } else if (t.find("Privacy") != std::string::npos) {
            saw_privacy = true;
        }
    }
    flush();
    return out;
}

// Parse `iw dev <if> link` → association snapshot. Output is "Not connected."
// when down, else "Connected to aa:bb:.. (on wlanX)" + "SSID: <name>".
inline void parse_link(const std::string& text, WifiObservation& obs) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.rfind("Connected to", 0) == 0) {
            obs.associated = true;
            std::string rest = trim(t.substr(12));
            obs.assoc_bssid = trim(rest.substr(0, rest.find('(')));
        } else if (t.rfind("SSID:", 0) == 0) {
            obs.assoc_ssid = unescape_iw(trim(t.substr(5)));   // \xNN → real bytes
        }
    }
}

}  // namespace nm_detail

// Resolve the wifi interface to query: explicit `want` if it's wireless, else
// auto-pick the first wireless link. "" if there's no wireless interface.
inline std::string wifi_iface(const std::string& want) {
    using namespace nm_detail;
    if (!want.empty()) return is_wireless(want) ? want : std::string();
    return first_wireless();
}

// CHEAP association-only probe (`iw dev <if> link`, no radio scan). Used by the
// poller on every tick to drive the WifiAssociated/WifiDisassociated edges
// without triggering a costly scan each second. Returns false if there's no
// wireless interface to query.
inline bool wifi_assoc_probe(const std::string& want, WifiObservation& obs) {
    using namespace nm_detail;
    std::string iface = wifi_iface(want);
    if (iface.empty()) return false;
    obs.interface = iface;
    std::string link_out;
    if (run_capture("iw dev " + iface + " link", link_out))
        parse_link(link_out, obs);
    return true;
}

// FULL observation: association snapshot + a fresh scan. The on-demand path
// behind the WifiScan operation (tdb/rtdb `wifi`) — NOT the per-tick path.
// `iw dev <if> scan` triggers an active scan (needs CAP_NET_ADMIN); falls back
// to `scan dump` (the cached last scan) when the trigger is denied/busy.
inline WifiObservation wifi_observe(const std::string& want) {
    using namespace nm_detail;
    WifiObservation obs;
    if (!wifi_assoc_probe(want, obs)) return obs;   // no wireless iface

    std::string scan_out;
    if (run_capture("iw dev " + obs.interface + " scan", scan_out) &&
        !scan_out.empty())
        obs.bss = parse_scan(scan_out);
    if (obs.bss.empty() &&
        run_capture("iw dev " + obs.interface + " scan dump", scan_out))
        obs.bss = parse_scan(scan_out);

    return obs;
}

// ─── VPN tunnel observation (Headscale/Tailscale) ──────────────────────────
//
// NM OBSERVES the remote tunnel; tailscaled (driven by Headscale) does the
// WireGuard handshake. The cheap per-tick probe asks `tailscale status --json`
// for the backend state — "Running" with a Self.Online tunnel means the rig has
// remote connectivity. Falls back to checking the wg interface is up + has a
// peer (`wg show <if> latest-handshakes`) when the tailscale CLI is absent.
// Returns false only when there's no VPN mechanism present at all.
struct VpnObservation {
    std::string interface;   // the tunnel iface ("tailscale0" / wg iface), "" if none
    bool        up = false;  // tunnel established (remote connectivity available)
    std::string note;        // human detail for the status line
};

// Probe the Tailscale/WireGuard tunnel. `want` = the configured vpn_interface
// ("" → "tailscale0"). `up` is true when tailscaled reports BackendState
// "Running" (the tunnel is established).
inline bool vpn_observe(const std::string& want, VpnObservation& obs) {
    using namespace nm_detail;
    obs.interface = want.empty() ? "tailscale0" : want;

    // Primary: the tailscale backend state. We don't pull a JSON parser into the
    // FC for one field — grep the line `"BackendState": "Running"`. tailscale
    // prints it whether or not jq is present.
    std::string out;
    if (run_capture("tailscale status --json", out) && !out.empty()) {
        const bool running = out.find("\"BackendState\": \"Running\"") != std::string::npos
                          || out.find("\"BackendState\":\"Running\"")  != std::string::npos;
        // "Running" + at least one Self address means the tunnel is up.
        const bool has_self = out.find("\"TailscaleIPs\"") != std::string::npos;
        obs.up = running && has_self;
        obs.note = obs.up ? "tailscale Running" : "tailscale not Running";
        return true;
    }

    // Fallback: a raw wg interface with a recent handshake (non-zero epoch).
    if (run_capture("wg show " + obs.interface + " latest-handshakes", out) &&
        !out.empty()) {
        // Any peer line with a non-"0" handshake timestamp → tunnel up.
        bool handshook = false;
        size_t pos = 0;
        while (pos < out.size()) {
            size_t nl = out.find('\n', pos);
            std::string line = out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? out.size() : nl + 1;
            // "<pubkey>\t<epoch>"; up if the trailing number is > 0.
            size_t tab = line.find_last_of(" \t");
            if (tab != std::string::npos) {
                std::string ts = trim(line.substr(tab + 1));
                if (!ts.empty() && ts != "0") { handshook = true; break; }
            }
        }
        obs.up = handshook;
        obs.note = handshook ? "wg handshake present" : "wg no handshake";
        return true;
    }

    obs.note = "no tailscale/wg present";
    return false;   // no VPN mechanism at all
}

// Result of a CONNECT drive (wifi or vpn): ok + a human note for the log/status.
struct ConnectResult {
    bool        ok = false;
    std::string note;
};

// ─── VPN CONNECT path (drive `tailscale up`, pinned to the WiFi underlay) ──
//
// Symmetric to wifi_connect: NM doesn't embed WireGuard, it DRIVES tailscaled.
// Once the WiFi link has an address, bring the tunnel up so it rides WiFi.
//
// "On the WiFi interface" = the tunnel UNDERLAY egresses over `wifi_iface`.
// tailscaled binds its underlay socket to whichever interface owns the route to
// the control plane. NM only reaches this path once WiFi is associated AND has
// an address (the IP_ACQUIRED rung over WiFi), so WiFi is the live default-route
// link and a plain `tailscale up` already rides it. To make that robust against
// a STALE wired route lingering at higher priority, we (best-effort) ensure the
// WiFi link's default route wins before `up` by lowering its metric — a no-op if
// WiFi is already the only/default route. We never touch the eth lifeline route.
//
// --accept-routes=false / --accept-dns=false keep the tunnel from hijacking the
// rig's LAN routes; it only adds the 100.64/10 overlay. The authkey is OPTIONAL:
// an already-enrolled node re-ups from its stored node key without one.
inline ConnectResult vpn_connect(const std::string& wifi_iface,
                                 const std::string& authkey) {
    using namespace nm_detail;
    ConnectResult r;

    // No tailscale binary → nothing to drive (observe-only fallback applies).
    std::string probe;
    if (!run_capture("tailscale version", probe) || probe.empty()) {
        r.note = "tailscale not installed";
        return r;
    }

    // Best-effort: make the WiFi link the preferred default so tailscaled's
    // underlay binds to it. Adds a low-metric default via WiFi's own gateway;
    // harmless + idempotent if WiFi is already the default. (We do NOT delete any
    // existing route — the eth lifeline stays reachable on its own subnet route.)
    std::string ign;
    if (!wifi_iface.empty()) {
        std::string gw;
        if (run_capture("ip -o route show default dev " + wifi_iface, gw) &&
            !gw.empty())
            run_capture("ip route replace default dev " + wifi_iface +
                        " metric 50", ign);
    }

    std::string cmd = "tailscale up --accept-routes=false --accept-dns=false";
    if (!authkey.empty())
        cmd += " --authkey '" + authkey + "'";

    std::string out;
    const bool ok = run_capture(cmd, out);
    r.ok = ok;
    r.note = ok ? ("tailscale up (underlay via " +
                   (wifi_iface.empty() ? std::string("default") : wifi_iface) + ")")
                : ("tailscale up failed: " + trim(out));
    return r;
}

// ─── WiFi CONNECT path (drive wpa_supplicant + DHCP) ───────────────────────
//
// NM does NOT embed 802.11 — it DRIVES the standard Linux wifi stack. The
// BUILDROOT-portable lowest common denominator is `wpa_cli` (the wpa_supplicant
// control interface, present on Buildroot/Debian/the Pi alike) for association +
// a DHCP client (udhcpc on Buildroot, dhclient on Debian) for the lease. NOT
// nmcli/NetworkManager — that's a desktop/Debian convenience, not the deployment
// target. wpa_supplicant must already be running with a control socket on the
// iface (the standard Buildroot init does this; `wpa_cli` finds it under
// /var/run/wpa_supplicant/<iface>).

struct WifiProfileInfo {
    std::string ssid;
    std::string psk;        // WPA2 passphrase ("" = open network)
    uint32_t    priority = 0;
};

// Pick the best profile to connect: the highest-priority configured profile whose
// SSID is currently VISIBLE in the scan. Returns the index into `profiles`, or -1
// if none of the known SSIDs are in range. (Ethernet-preferred policy is decided
// by the caller — it only reaches here when there's no usable wired link.)
inline int pick_wifi_profile(const std::vector<WifiProfileInfo>& profiles,
                             const std::vector<WifiBssInfo>& visible) {
    int best = -1;
    uint32_t best_prio = 0;
    for (size_t i = 0; i < profiles.size(); ++i) {
        bool in_range = false;
        for (const auto& b : visible)
            if (b.ssid == profiles[i].ssid) { in_range = true; break; }
        if (!in_range) continue;
        if (best < 0 || profiles[i].priority > best_prio) {
            best = static_cast<int>(i);
            best_prio = profiles[i].priority;
        }
    }
    return best;
}

// Run a DHCP client to acquire a lease on `iface`. Tries udhcpc (Buildroot) then
// dhclient (Debian); one-shot, short timeout. Best-effort — the poller's netlink
// observation is what actually confirms the address landed.
inline bool dhcp_acquire(const std::string& iface) {
    using namespace nm_detail;
    std::string out;
    // udhcpc -i <if> -n (exit if lease fails) -q (exit after obtaining) -t 3.
    if (run_capture("command -v udhcpc", out) && !trim(out).empty()) {
        run_capture("udhcpc -i " + iface + " -n -q -t 3 -T 2", out);
        return true;
    }
    if (run_capture("command -v dhclient", out) && !trim(out).empty()) {
        run_capture("dhclient -1 -timeout 8 " + iface, out);
        return true;
    }
    return false;   // no DHCP client — a static-IP deployment handles addressing
}

// Associate `iface` to `ssid` (WPA2 if psk non-empty, else open) by driving
// wpa_cli, then kick a DHCP lease. The wpa_cli sequence is the canonical one:
//   add_network → set_network ssid/psk/key_mgmt → enable_network → select_network
// Returns ok on a successful wpa_cli select (association completes asynchronously;
// the poller observes the resulting assoc + addr edges).
inline ConnectResult wifi_connect(const std::string& iface,
                                  const std::string& ssid,
                                  const std::string& psk) {
    using namespace nm_detail;
    ConnectResult r;
    if (iface.empty() || ssid.empty()) { r.note = "no iface/ssid"; return r; }
    const std::string wc = "wpa_cli -i " + iface + " ";

    std::string out;
    if (!run_capture(wc + "status", out) || out.empty()) {
        r.note = "wpa_supplicant not reachable (no control socket on " + iface + ")";
        return r;
    }

    // A fresh network slot. wpa_cli prints the new network id on add_network.
    std::string id_s;
    if (!run_capture(wc + "add_network", id_s)) { r.note = "add_network failed"; return r; }
    const std::string id = trim(id_s);
    if (id.empty() || id == "FAIL") { r.note = "add_network → FAIL"; return r; }

    // SSID must be quoted in wpa_cli set_network.
    run_capture(wc + "set_network " + id + " ssid '\"" + ssid + "\"'", out);
    if (psk.empty()) {
        run_capture(wc + "set_network " + id + " key_mgmt NONE", out);   // open
    } else {
        run_capture(wc + "set_network " + id + " psk '\"" + psk + "\"'", out);
    }
    run_capture(wc + "enable_network " + id, out);
    std::string sel;
    run_capture(wc + "select_network " + id, sel);
    if (trim(sel).find("OK") == std::string::npos) {
        r.note = "select_network → " + trim(sel);
        return r;
    }
    // Persist the running config so the association survives a wpa restart.
    run_capture(wc + "save_config", out);

    dhcp_acquire(iface);   // best-effort lease; poller confirms the address edge.
    r.ok = true;
    r.note = "associating to '" + ssid + "' (id " + id + ")";
    return r;
}

}  // namespace ara::nm

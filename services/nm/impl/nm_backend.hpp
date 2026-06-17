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
        if (t.rfind("BSS ", 0) == 0) {
            flush();
            cur = WifiBssInfo{};
            have = true;
            saw_rsn = saw_wpa = saw_sae = saw_privacy = false;
            // "BSS aa:bb:cc:dd:ee:ff(on wlan0)" — take the MAC token.
            std::string rest = t.substr(4);
            cur.bssid = trim(rest.substr(0, rest.find('(')));
        } else if (t.rfind("SSID:", 0) == 0) {
            cur.ssid = trim(t.substr(5));
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
            obs.assoc_ssid = trim(t.substr(5));
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

}  // namespace ara::nm

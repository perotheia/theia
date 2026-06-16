// nm_backend — the THIN read-only link/address probe behind NmPoller.
//
// APP-OWNED. NM is a control plane, NOT a network configurator: this backend
// only OBSERVES. It shells `ip -o link show` + `ip -o addr show` (the iproute2
// CLI, no library / data path) and reports two booleans for the monitored
// interface: has_carrier (the link's LOWER_UP flag) and has_address (a routable
// `scope global` inet/inet6 is present). The kernel + systemd-networkd own the
// actual configuration; we never touch it.
//
// Interface selection: an explicit name (from NmConfig.interfaces, first of a
// comma list) is honored; "" = auto — the first non-loopback link that has
// carrier (so an unplugged secondary NIC doesn't hold readiness DOWN).
//
// Each probe is a short popen; on any failure we degrade to "no carrier / no
// address" (DOWN) rather than throwing — a missing `ip` or a denied read must
// not crash the FC.

#pragma once

#include <cstdio>
#include <cstring>
#include <string>

namespace ara::nm {

struct LinkObservation {
    std::string interface;   // the link actually observed ("" if none found)
    bool has_carrier = false;
    bool has_address = false;
};

namespace nm_detail {

// Run a command, return its full stdout (empty on failure). Read-only queries
// only — never a configuring command.
inline std::string run_(const std::string& cmd) {
    std::string out;
    FILE* p = ::popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!p) return out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    ::pclose(p);
    return out;
}

// Does interface `iface` carry a routable (scope global) inet/inet6 address?
inline bool has_global_addr_(const std::string& iface) {
    // `ip -o addr show dev <iface> scope global` prints one line per global
    // address; any output → a routable address exists.
    std::string out = run_("ip -o addr show dev " + iface + " scope global");
    return !out.empty();
}

}  // namespace nm_detail

// Probe the monitored interface. `want` is the desired interface name ("" =
// auto-pick the first non-loopback link with carrier). require_address gates
// has_address into the readiness; the poller decides what edges to emit.
inline LinkObservation probe_link(const std::string& want) {
    using namespace nm_detail;
    LinkObservation obs;

    // `ip -o link show` — one line per link. We scan for the wanted interface
    // (or, in auto mode, the first non-lo link flagged LOWER_UP).
    std::string links = run_("ip -o link show");
    if (links.empty()) return obs;   // no `ip` / no links → DOWN

    std::size_t pos = 0;
    while (pos < links.size()) {
        std::size_t eol = links.find('\n', pos);
        std::string line = links.substr(pos, eol == std::string::npos
                                              ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? links.size() : eol + 1;
        if (line.empty()) continue;

        // Line shape: "<idx>: <name>: <FLAGS> mtu ...". Extract <name>.
        std::size_t c1 = line.find(": ");
        if (c1 == std::string::npos) continue;
        std::size_t c2 = line.find(':', c1 + 2);
        if (c2 == std::string::npos) continue;
        std::string name = line.substr(c1 + 2, c2 - (c1 + 2));
        // Trim a trailing "@parent" (e.g. vlan "eth0.10@eth0").
        std::size_t at = name.find('@');
        if (at != std::string::npos) name = name.substr(0, at);
        if (name == "lo") continue;                       // never the loopback

        const bool carrier = line.find("LOWER_UP") != std::string::npos;

        if (!want.empty()) {
            if (name != want) continue;                   // explicit: only it
            obs.interface = name;
            obs.has_carrier = carrier;
            obs.has_address = carrier && has_global_addr_(name);
            return obs;
        }
        // Auto mode: first non-lo link WITH carrier wins.
        if (carrier) {
            obs.interface = name;
            obs.has_carrier = true;
            obs.has_address = has_global_addr_(name);
            return obs;
        }
        // Remember the first non-lo link even if down, so we report a name
        // (DOWN) rather than "" when nothing has carrier.
        if (obs.interface.empty()) obs.interface = name;
    }
    return obs;   // explicit name not found, or no carrier anywhere → as-built
}

}  // namespace ara::nm

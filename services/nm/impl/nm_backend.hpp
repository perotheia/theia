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
#include <cstring>
#include <map>
#include <set>
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

}  // namespace ara::nm

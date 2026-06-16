// netlink_diag — listening-socket inventory via raw NETLINK_SOCK_DIAG.
//
// APP-OWNED. Replaces the `ss -Htlnp` popen in proc_detector: this is what `ss`
// itself does — an AF_NETLINK / NETLINK_SOCK_DIAG request (SOCK_DIAG_BY_FAMILY,
// INET_DIAG_REQ_V2) filtered to TCP_LISTEN, parsed from struct inet_diag_msg.
// No fork, no text scraping; structs straight from the kernel. Uses only the
// already-present kernel uapi headers (linux/sock_diag.h, inet_diag.h) — no
// libmnl/libnl dependency (raw netlink is a plain socket).
//
// The kernel gives each socket's inode (idiag_inode), not its pid. We map
// inode→(pid,comm) by scanning ONLY the supervisor's children's /proc/<pid>/fd/
// (the deployment's FCs — the same scope the detector uses), so the join is
// cheap (~16 procs) and a listener owned by a non-FC host process is simply not
// in the map → ignored, which is exactly the scoping we want.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>      // IPPROTO_TCP
#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <linux/inet_diag.h>
#include <netinet/tcp.h>     // TCP_LISTEN

namespace ara::idsm {
namespace nl_detail {

// inode → (pid, comm) for every socket fd held by a child of `supervisor_pid`.
// /proc/<pid>/fd/<n> is a symlink "socket:[<inode>]" for socket fds.
inline std::map<uint64_t, std::pair<int, std::string>>
fc_socket_inodes(int supervisor_pid) {
    std::map<uint64_t, std::pair<int, std::string>> out;
    DIR* proc = ::opendir("/proc");
    if (!proc) return out;
    struct dirent* e;
    while ((e = ::readdir(proc)) != nullptr) {
        char* end = nullptr;
        long pid = std::strtol(e->d_name, &end, 10);
        if (end == e->d_name || *end) continue;            // not a pid dir
        // scope: only the supervisor's children (the FCs).
        {
            char sp[64];
            std::snprintf(sp, sizeof(sp), "/proc/%ld/stat", pid);
            FILE* sf = ::fopen(sp, "r");
            if (!sf) continue;
            char sbuf[256]; size_t n = ::fread(sbuf, 1, sizeof(sbuf) - 1, sf);
            ::fclose(sf); sbuf[n] = '\0';
            char* rp = std::strrchr(sbuf, ')');
            int ppid = -1, stdummy;
            if (!rp || std::sscanf(rp + 1, " %c %d", (char*)&stdummy, &ppid) < 2
                || ppid != supervisor_pid)
                continue;
        }
        // comm
        std::string comm;
        {
            char cp[64];
            std::snprintf(cp, sizeof(cp), "/proc/%ld/comm", pid);
            FILE* cf = ::fopen(cp, "r");
            if (cf) { char cb[64]; if (std::fgets(cb, sizeof(cb), cf)) {
                comm = cb; while (!comm.empty() &&
                    (comm.back() == '\n' || comm.back() == '\r')) comm.pop_back();
            } ::fclose(cf); }
        }
        // fd scan → socket inodes
        char fdp[64];
        std::snprintf(fdp, sizeof(fdp), "/proc/%ld/fd", pid);
        DIR* fd = ::opendir(fdp);
        if (!fd) continue;
        struct dirent* fe;
        while ((fe = ::readdir(fd)) != nullptr) {
            char link[96], tgt[96];
            std::snprintf(link, sizeof(link), "%s/%s", fdp, fe->d_name);
            ssize_t l = ::readlink(link, tgt, sizeof(tgt) - 1);
            if (l <= 0) continue;
            tgt[l] = '\0';
            // "socket:[12345]"
            if (std::strncmp(tgt, "socket:[", 8) != 0) continue;
            uint64_t inode = std::strtoull(tgt + 8, nullptr, 10);
            out[inode] = {static_cast<int>(pid), comm};
        }
        ::closedir(fd);
    }
    ::closedir(proc);
    return out;
}

// One sock_diag request for a family (AF_INET/AF_INET6), TCP listeners only.
// Appends (port, inode) for each listening socket to `out`.
inline void diag_listeners(int fd, uint8_t family,
                           std::vector<std::pair<int, uint64_t>>& out) {
    struct {
        struct nlmsghdr nlh;
        struct inet_diag_req_v2 req;
    } msg{};
    msg.nlh.nlmsg_len   = sizeof(msg);
    msg.nlh.nlmsg_type  = SOCK_DIAG_BY_FAMILY;
    msg.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    msg.nlh.nlmsg_seq   = 1;
    msg.req.sdiag_family   = family;
    msg.req.sdiag_protocol = IPPROTO_TCP;
    msg.req.idiag_states   = (1u << TCP_LISTEN);
    if (::send(fd, &msg, sizeof(msg), 0) < 0) return;

    char buf[8192];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return;
        for (struct nlmsghdr* nlh = (struct nlmsghdr*)buf;
             NLMSG_OK(nlh, (size_t)n); nlh = NLMSG_NEXT(nlh, n)) {
            if (nlh->nlmsg_type == NLMSG_DONE) return;
            if (nlh->nlmsg_type == NLMSG_ERROR) return;
            auto* dm = (struct inet_diag_msg*)NLMSG_DATA(nlh);
            // idiag_sport is big-endian.
            uint16_t port = (uint16_t)((dm->id.idiag_sport >> 8) |
                                       (dm->id.idiag_sport << 8));
            out.emplace_back((int)port, (uint64_t)dm->idiag_inode);
        }
    }
}

}  // namespace nl_detail

// One observed listening socket (same shape proc_detector used from `ss`).
struct NlListener {
    std::string comm;
    int         pid = -1;
    int         port = 0;
};

// Listening TCP sockets owned by the Theia FCs (children of `supervisor_pid`),
// via NETLINK_SOCK_DIAG — no `ss` exec. Joins the kernel's (port, inode) list to
// the FC inode→pid map; a listener not owned by an FC is dropped (scope).
inline std::vector<NlListener> netlink_listeners(int supervisor_pid) {
    std::vector<NlListener> out;
    int fd = ::socket(AF_NETLINK, SOCK_RAW, NETLINK_SOCK_DIAG);
    if (fd < 0) return out;
    std::vector<std::pair<int, uint64_t>> socks;   // (port, inode)
    nl_detail::diag_listeners(fd, AF_INET, socks);
    nl_detail::diag_listeners(fd, AF_INET6, socks);
    ::close(fd);

    auto inodes = nl_detail::fc_socket_inodes(supervisor_pid);
    for (const auto& [port, inode] : socks) {
        auto it = inodes.find(inode);
        if (it == inodes.end()) continue;          // not an FC socket → ignore
        NlListener l;
        l.port = port; l.pid = it->second.first; l.comm = it->second.second;
        out.push_back(std::move(l));
    }
    return out;
}

}  // namespace ara::idsm

// User do_* bodies for the runnable node TraceStreamPump — the trace hot path.
//
// HAND-OWNED. The pump owns its thread + its own AF_TIPC/SOCK_SEQPACKET listen
// loop on in_records (0x80010013, the address every node's Tracer submits to).
// For each inbound TraceRecord frame it pulls the RAW proto-wire payload (never
// decodes — TraceRecord strings/bytes are nanopb pb_callback fields) and hands
// it to the process-global TraceHub: ring + fan-out to subscribers. The atomic
// TraceCtl node feeds the same hub from the Subscribe side.
//
// GenRunnable has no State struct, so the listen fd is a do_loop() local; the
// loop exits cooperatively on stop_requested() (the 100ms select timeout makes
// it responsive without a wake socket).
//
// See docs/tasks/TODO/composition-isolation-test.md.

#include "lib/TraceStreamPump.hh"

#include <sys/socket.h>
#include <linux/tipc.h>
#include <unistd.h>
#include <sys/select.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include "TheiaMsgHeader.hh"
#include "impl/trace_hub.hpp"

namespace ara::log {

namespace {
constexpr int kRecvBuf = 4096;

int bind_listen(uint32_t type, uint32_t instance) {
    int fd = ::socket(AF_TIPC, SOCK_SEQPACKET, 0);
    if (fd < 0) { std::perror("[trace_pump] socket"); return -1; }
    struct sockaddr_tipc addr{};
    addr.family   = AF_TIPC;
    addr.addrtype = TIPC_SERVICE_ADDR;
    addr.scope    = TIPC_NODE_SCOPE;
    addr.addr.name.name.type     = type;
    addr.addr.name.name.instance = instance;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("[trace_pump] bind"); ::close(fd); return -1;
    }
    if (::listen(fd, 16) < 0) {
        std::perror("[trace_pump] listen"); ::close(fd); return -1;
    }
    return fd;
}
}  // namespace

void TraceStreamPump::do_start() {
    std::fprintf(stderr, "[%s] trace pump starting (bind 0x%08x)\n",
                 kNodeName, kTipcType);
}

void TraceStreamPump::do_loop() {
    int listen_fd = bind_listen(kTipcType, kTipcInstance);
    if (listen_fd < 0) {
        std::fprintf(stderr, "[%s] FAILED to bind 0x%08x — no traces\n",
                     kNodeName, kTipcType);
        return;
    }
    std::vector<int> conns;
    uint8_t buf[kRecvBuf];
    while (!stop_requested()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;
        for (int c : conns) { FD_SET(c, &rfds); if (c > maxfd) maxfd = c; }
        timeval tv{0, 100 * 1000};  // 100ms — keeps stop responsive
        int n = ::select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        if (n < 0) { if (errno == EINTR) continue; break; }

        if (FD_ISSET(listen_fd, &rfds)) {
            int c = ::accept(listen_fd, nullptr, nullptr);
            if (c >= 0) conns.push_back(c);
        }
        for (auto it = conns.begin(); it != conns.end();) {
            int c = *it;
            if (!FD_ISSET(c, &rfds)) { ++it; continue; }
            ssize_t r = ::recv(c, buf, sizeof(buf), 0);
            if (r <= 0) { ::close(c); it = conns.erase(it); continue; }
            if (r < (ssize_t)sizeof(::theia::runtime::TheiaMsgHeader)) {
                ++it; continue;
            }
            ::theia::runtime::TheiaMsgHeader hdr{};
            std::memcpy(&hdr, buf, sizeof(hdr));
            // Raw record bytes follow the 24-byte header. Forward verbatim.
            const uint8_t* payload = buf + sizeof(hdr);
            TraceHub::instance().submit(
                std::string(reinterpret_cast<const char*>(payload),
                            hdr.proto_len));
            ++it;
        }
    }
    for (int c : conns) ::close(c);
    ::close(listen_fd);
    std::fprintf(stderr, "[%s] trace pump loop exiting\n", kNodeName);
}

void TraceStreamPump::do_stop() {
    // Cooperative: stop_requested() is set by the base; the select timeout
    // makes do_loop() notice within ~100ms and tear down its own fds.
    std::fprintf(stderr, "[%s] trace pump stopping\n", kNodeName);
}

}  // namespace ara::log

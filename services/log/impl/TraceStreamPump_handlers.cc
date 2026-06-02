// User do_* bodies for the runnable node TraceStreamPump — the trace hot path.
//
// HAND-OWNED. The pump owns its thread + its own AF_TIPC/SOCK_DGRAM receive
// loop on in_records (0x80010013, the address every node's Tracer submits to).
// For each inbound TraceRecord datagram it pulls the RAW proto-wire payload
// (never decodes — TraceRecord strings/bytes are nanopb pb_callback fields) and
// hands it to the process-global TraceHub: ring + fan-out to subscribers. The
// atomic TraceCtl node feeds the same hub from the Subscribe side.
//
// SOCK_DGRAM (NOT SEQPACKET): the sender side — Tracer::TraceSubmitter — is a
// connectionless SOCK_DGRAM that sendto()s the collector's TIPC name per record
// (lossy-OK firehose, never blocks the dispatch thread). The two MUST agree on
// socket type — a DGRAM sendto() to a SEQPACKET listener is silently dropped by
// the kernel (no connection, wrong type), which is exactly why records never
// arrived. So the pump binds DGRAM and recvfrom()s; no listen/accept.
//
// GenRunnable has no State struct, so the bound fd is a do_loop() local; the
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

#include "TheiaMsgHeader.hh"
#include "impl/trace_hub.hpp"

namespace ara::log {

namespace {
constexpr int kRecvBuf = 4096;

// Bind the collector's TIPC service name as a connectionless SOCK_DGRAM
// receiver — matches Tracer::TraceSubmitter's SOCK_DGRAM sendto() sender.
int bind_dgram(uint32_t type, uint32_t instance) {
    int fd = ::socket(AF_TIPC, SOCK_DGRAM, 0);
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
    // No listen()/accept() — DGRAM is connectionless; recvfrom() on this fd.
    return fd;
}
}  // namespace

void TraceStreamPump::do_start() {
    std::fprintf(stderr, "[%s] trace pump starting (bind 0x%08x)\n",
                 kNodeName, kTipcType);
}

void TraceStreamPump::do_loop() {
    int fd = bind_dgram(kTipcType, kTipcInstance);
    if (fd < 0) {
        std::fprintf(stderr, "[%s] FAILED to bind 0x%08x — no traces\n",
                     kNodeName, kTipcType);
        return;
    }
    uint8_t buf[kRecvBuf];
    while (!stop_requested()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{0, 100 * 1000};  // 100ms — keeps stop responsive
        int n = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0 || !FD_ISSET(fd, &rfds)) continue;

        // One datagram = one record frame: [TheiaMsgHeader][raw proto-wire].
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r < (ssize_t)sizeof(::theia::runtime::TheiaMsgHeader)) continue;
        ::theia::runtime::TheiaMsgHeader hdr{};
        std::memcpy(&hdr, buf, sizeof(hdr));
        // Raw record bytes follow the 24-byte header. Forward verbatim — never
        // decode (TraceRecord strings/bytes are nanopb pb_callback fields).
        const uint8_t* payload = buf + sizeof(hdr);
        // Guard proto_len against a short/truncated datagram.
        size_t avail = static_cast<size_t>(r) - sizeof(hdr);
        size_t plen  = hdr.proto_len <= avail ? hdr.proto_len : avail;
        TraceHub::instance().submit(
            std::string(reinterpret_cast<const char*>(payload), plen));
    }
    ::close(fd);
    std::fprintf(stderr, "[%s] trace pump loop exiting\n", kNodeName);
}

void TraceStreamPump::do_stop() {
    // Cooperative: stop_requested() is set by the base; the select timeout
    // makes do_loop() notice within ~100ms and tear down its own fds.
    std::fprintf(stderr, "[%s] trace pump stopping\n", kNodeName);
}

}  // namespace ara::log

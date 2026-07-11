// User do_* bodies for the runnable node TraceStreamPump — the trace hot path.
//
// HAND-OWNED. The pump owns its thread + its own AF_TIPC/SOCK_DGRAM receive
// loop on in_records (Tracer::TraceSubmitter::kCollectorTipcType — the address
// every node's Tracer submits to). That ingest address is DISTINCT from this
// node's .art address (0x80010013): the node's SEQPACKET mux binding publishes
// that name too, and TIPC anycasts a datagram across ALL publications of a
// name — a record routed to the SEQPACKET publication is silently dropped
// (was a 100%-dead firehose). One name per socket, never shared across types.
// For each inbound TraceRecord datagram it pulls the RAW proto-wire payload
// (never decodes — TraceRecord strings/bytes are nanopb pb_callback fields) and
// PG-MULTICASTS it VERBATIM to the TraceRecord group: one TIPC name-sequence
// datagram → the kernel fans out a copy to every observer that pg_joined. No
// ring, no Subscribe RPC, no per-subscriber unicast registry (all removed —
// tracecat is a LIVE tail; the supervisor allocates each observer's address).
//
// SOCK_DGRAM (NOT SEQPACKET) on the INGEST side: Tracer::TraceSubmitter is a
// connectionless SOCK_DGRAM that sendto()s the collector's name per record
// (lossy-OK firehose). The pump binds DGRAM and recvfrom()s; no listen/accept.
//
// The EGRESS side is PgClient: resolve the TraceRecord group's type once (a CALL
// to the supervisor allocator — log[] is a normal FC here, NOT the allocator,
// so the self-CALL hazard does not apply), then multicast each record by raw
// bytes (PgClient::broadcast takes the encoded payload + service_id — no decode).
//
// GenRunnable has no State struct, so the bound fd + PgClient are do_loop()
// locals; the loop exits cooperatively on stop_requested() (the 100ms select
// timeout makes it responsive without a wake socket).

#include "lib/TraceStreamPump.hh"

#include <sys/socket.h>
#include <linux/tipc.h>
#include <unistd.h>
#include <sys/select.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "TheiaMsgHeader.hh"
#include "PgClient.hh"          // PG name-sequence multicast egress
#include "Tracer.hh"            // kCollectorTipcType/-Instance — the ingest addr
                                // (single source shared with every submitter)
#include "lib/log_codecs.hh"    // RemoteCodec<system_services_log_TraceRecord>

namespace ara::log {

// The TraceRecord wire type — its NAME is the PG group identity (the same
// msg_type_name<T>() an observer's pg_join uses).
using TraceRecord = system_services_log_TraceRecord;

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
    std::fprintf(stderr, "[%s] trace pump starting (ingest bind 0x%08x)\n",
                 kNodeName,
                 ::theia::runtime::TraceSubmitter::kCollectorTipcType);
}

void TraceStreamPump::do_loop() {
    int fd = bind_dgram(::theia::runtime::TraceSubmitter::kCollectorTipcType,
                        ::theia::runtime::TraceSubmitter::kCollectorTipcInstance);
    if (fd < 0) {
        std::fprintf(stderr, "[%s] FAILED to bind ingest 0x%08x — no traces\n",
                     kNodeName,
                     ::theia::runtime::TraceSubmitter::kCollectorTipcType);
        return;
    }
    // PG egress: the pump is the TraceRecord group's broadcaster. Resolve the
    // group_type LAZILY on the first record (the supervisor may not be up at
    // boot); re-resolve until it succeeds. log[] is a normal FC (not the
    // allocator), so this CALL is fine.
    ::theia::runtime::PgClient pg;
    pg.attach(kNodeName, /*binding=*/nullptr);   // pure broadcaster: no recv side
    uint32_t group_type = 0;

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

        if (group_type == 0) {                       // not yet resolved
            auto g = pg.resolve<TraceRecord>();
            if (!g.ok) continue;                     // supervisor down — drop record
            group_type = g.type;
        }
        // ONE name-sequence multicast → every observer that pg_joined the group.
        pg.broadcast<TraceRecord>(group_type, payload,
                                  static_cast<uint16_t>(plen));
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

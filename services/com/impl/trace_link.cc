// trace_link implementation — SEQPACKET subscriber to log[trace]'s TraceCtl +
// a recv thread that hands each record's raw bytes to the sink.
//
// The nanopb world (SubscribeReq encode via RemoteRef, the reply-pump TipcMux)
// lives ONLY here, so trace_link.hpp's primitive surface keeps the libprotobuf
// gRPC edge in TraceForwarder_handlers.cc free of the nanopb log headers.

#include "impl/trace_link.hpp"

#include "NodeRef.hh"        // RemoteRef, call<>, TipcClient
#include "TipcMux.hh"        // reply pump for the SubscribeReq RemoteRef
#include "RemoteCodec.hh"    // hash_msg_type_ (kRecordServiceId)
#include "TheiaMsgHeader.hh"
#include "system/services/log/log.pb.h"   // nanopb SubscribeReq / TraceEmpty
#include "lib/log_codecs.hh"              // RemoteCodec<SubscribeReq/TraceEmpty>

#include <sys/socket.h>
#include <sys/select.h>
#include <linux/tipc.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace services_com {

namespace {

// log[trace]'s TraceCtl (subscription control) — system/services/log:
// `node atomic TraceCtl { tipc type=0x80010014 instance=0 }`.
constexpr uint32_t kTraceCtlType     = 0x80010014u;
constexpr uint32_t kTraceCtlInstance = 0u;

// Our subscriber listener address. The hub TipcClient::connect()s to this and
// pushes TraceRecord casts. A distinct type clear of the FC range + the com
// node addresses; instance pins to our pid so multiple processes that subscribe
// don't collide on the TIPC name.
constexpr uint32_t kSubType = 0x80010015u;

// The service_id the hub stamps on each fan-out cast — the SAME djb2 the hub
// computes from the TraceRecord type name (trace_hub.hpp kRecordServiceId).
const uint16_t kRecordServiceId =
    ::theia::runtime::hash_msg_type_("system_services_log_TraceRecord");

// RemoteRef peer identity for the SubscribeReq gen_call (trace tag only).
struct TraceCtlTag {
    static constexpr const char* kNodeName = "trace_ctl";
};
using TraceCtlRef =
    theia::runtime::RemoteRef<TraceCtlTag, kTraceCtlType, kTraceCtlInstance>;

constexpr int kBacklog = 1;
constexpr int kRecvBuf = 8192;

}  // namespace

struct TraceLink::Impl {
    theia::runtime::TipcMux reply_mux;   // pumps the SubscribeReq call reply
    TraceCtlRef             ctl;
    TraceSink               sink;
    std::atomic<bool>       running{false};
    std::thread             rx_thread;
    int                     listen_fd = -1;
    uint32_t                sub_instance = 0;

    int bind_listen_() {
        int fd = ::socket(AF_TIPC, SOCK_SEQPACKET, 0);
        if (fd < 0) { std::perror("[trace_link] socket"); return -1; }
        struct sockaddr_tipc addr{};
        addr.family   = AF_TIPC;
        addr.addrtype = TIPC_SERVICE_ADDR;
        addr.scope    = TIPC_NODE_SCOPE;
        addr.addr.name.name.type     = kSubType;
        addr.addr.name.name.instance = sub_instance;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::perror("[trace_link] bind"); ::close(fd); return -1;
        }
        if (::listen(fd, kBacklog) < 0) {
            std::perror("[trace_link] listen"); ::close(fd); return -1;
        }
        return fd;
    }

    // Accept the hub's connection, then recv frames until stop. The hub spills
    // its ring backlog on subscribe, then live records follow. Each frame is
    // [TheiaMsgHeader][TraceRecord proto bytes]; we forward the payload verbatim
    // to the sink (the gRPC edge re-parses it).
    void rx_loop() {
        uint8_t buf[kRecvBuf];
        int conn = -1;
        while (running.load()) {
            if (conn < 0) {
                // Wait (bounded) for the hub to connect.
                fd_set rfds; FD_ZERO(&rfds); FD_SET(listen_fd, &rfds);
                struct timeval tv{0, 200 * 1000};
                int r = ::select(listen_fd + 1, &rfds, nullptr, nullptr, &tv);
                if (r <= 0) continue;
                conn = ::accept(listen_fd, nullptr, nullptr);
                if (conn < 0) continue;
                std::fprintf(stderr, "[trace_link] hub connected\n");
            }
            fd_set rfds; FD_ZERO(&rfds); FD_SET(conn, &rfds);
            struct timeval tv{0, 200 * 1000};
            int r = ::select(conn + 1, &rfds, nullptr, nullptr, &tv);
            if (r <= 0) continue;
            ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0) {                        // hub closed; re-accept
                ::close(conn); conn = -1;
                std::fprintf(stderr, "[trace_link] hub disconnected\n");
                continue;
            }
            if (n < (ssize_t)sizeof(::theia::runtime::TheiaMsgHeader)) continue;
            ::theia::runtime::TheiaMsgHeader hdr{};
            std::memcpy(&hdr, buf, sizeof(hdr));
            if (hdr.rpc.service_id != kRecordServiceId) continue;
            const uint8_t* payload = buf + sizeof(hdr);
            size_t avail = static_cast<size_t>(n) - sizeof(hdr);
            size_t plen  = hdr.proto_len <= avail ? hdr.proto_len : avail;
            if (sink) {
                sink(std::string(reinterpret_cast<const char*>(payload), plen));
            }
        }
        if (conn >= 0) ::close(conn);
    }
};

TraceLink::TraceLink() : impl_(new Impl()) {}
TraceLink::~TraceLink() { stop(); delete impl_; }

TraceLink& TraceLink::instance() {
    static TraceLink s;
    return s;
}

void TraceLink::set_sink(TraceSink sink) { impl_->sink = std::move(sink); }

bool TraceLink::start(int connect_timeout_ms) {
    if (impl_->running.load()) return true;

    // Pin the subscriber instance to our pid so co-resident subscribers don't
    // collide on the TIPC name.
    impl_->sub_instance =
        static_cast<uint32_t>(::getpid()) & 0xFFFFu;

    impl_->listen_fd = impl_->bind_listen_();
    if (impl_->listen_fd < 0) return false;

    // Start the recv thread FIRST so it's ready to accept the hub the moment
    // the subscribe call returns (the hub connects + spills backlog inline).
    impl_->running.store(true);
    impl_->rx_thread = std::thread([this] { impl_->rx_loop(); });

    // gen_call SubscribeReq to TraceCtl. Reply is TraceEmpty (just an ack).
    if (!impl_->ctl.connect(connect_timeout_ms)) {
        std::fprintf(stderr,
            "[trace_link] TraceCtl (0x%08x/0) unreachable; no trace egress\n",
            kTraceCtlType);
        stop();
        return false;
    }
    impl_->reply_mux.watch_remote_ref(impl_->ctl);
    impl_->reply_mux.start();

    system_services_log_SubscribeReq req =
        system_services_log_SubscribeReq_init_zero;
    req.sub_type     = kSubType;
    req.sub_instance = impl_->sub_instance;
    req.kind         = 0;        // all kinds; gRPC-side filtering is per-stream
    // target_node left empty (all nodes).

    auto result = theia::runtime::call<system_services_log_TraceEmpty>(
        impl_->ctl, req, /*act=*/0, connect_timeout_ms);
    if (result.tag != theia::runtime::CallTag::Reply) {
        std::fprintf(stderr,
            "[trace_link] SubscribeReq to TraceCtl failed (tag=%d)\n",
            static_cast<int>(result.tag));
        stop();
        return false;
    }
    std::fprintf(stderr,
        "[trace_link] subscribed to trace hub (sub 0x%08x/%u)\n",
        kSubType, impl_->sub_instance);
    return true;
}

void TraceLink::stop() {
    if (!impl_->running.exchange(false)) {
        // Even if never fully started, close a dangling listener.
        if (impl_->listen_fd >= 0) { ::close(impl_->listen_fd); impl_->listen_fd = -1; }
        return;
    }
    if (impl_->rx_thread.joinable()) impl_->rx_thread.join();
    impl_->reply_mux.stop();
    if (impl_->listen_fd >= 0) { ::close(impl_->listen_fd); impl_->listen_fd = -1; }
}

bool TraceLink::running() const { return impl_->running.load(); }

}  // namespace services_com

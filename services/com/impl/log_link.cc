// log_link implementation — SEQPACKET subscriber to log[logging]'s LogDaemon +
// a recv thread that hands each record's raw bytes to the sink.
//
// The nanopb world (LogSubscribeReq encode via RemoteRef, the reply-pump
// TipcMux) lives ONLY here, so log_link.hpp's primitive surface keeps the
// libprotobuf gRPC edge in LogForwarder_handlers.cc free of the nanopb log
// headers. Mirror of trace_link.cc.

#include "impl/log_link.hpp"

#include "NodeRef.hh"        // RemoteRef, call<>, TipcClient
#include "TipcMux.hh"        // reply pump for the LogSubscribeReq RemoteRef
#include "RemoteCodec.hh"    // hash_msg_type_ (kRecordServiceId)
#include "TheiaMsgHeader.hh"
#include "system/services/log/log.pb.h"   // nanopb LogSubscribeReq / LogEmpty
#include "lib/log_codecs.hh"              // RemoteCodec<LogSubscribeReq/LogEmpty>

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

// log[logging]'s LogDaemon (subscription control) — system/services/log:
// `node atomic LogDaemon { tipc type=0x80010003 instance=0 }`.
constexpr uint32_t kLogDaemonType     = 0x80010003u;
constexpr uint32_t kLogDaemonInstance = 0u;

// Our subscriber listener address. The hub TipcClient::connect()s to this and
// pushes LogRecord casts. Distinct from trace_link's kSubType (0x80010015) so
// the two egress links in one com process don't collide.
constexpr uint32_t kSubType = 0x80010025u;

// The service_id the hub stamps on each fan-out cast — the SAME djb2 the hub
// computes from the LogRecord type name (log_hub.hpp kRecordServiceId).
const uint16_t kRecordServiceId =
    ::theia::runtime::hash_msg_type_("system_services_log_LogRecord");

// RemoteRef peer identity for the LogSubscribeReq gen_call (trace tag only).
struct LogDaemonTag {
    static constexpr const char* kNodeName = "log_daemon";
};
using LogDaemonRef =
    theia::runtime::RemoteRef<LogDaemonTag, kLogDaemonType, kLogDaemonInstance>;

constexpr int kBacklog = 1;
constexpr int kRecvBuf = 9216;   // > BUFSIZ(8192) log line + header

}  // namespace

struct LogLink::Impl {
    theia::runtime::TipcMux reply_mux;   // pumps the LogSubscribeReq call reply
    LogDaemonRef            ctl;
    LogSink                 sink;
    std::atomic<bool>       running{false};
    std::thread             rx_thread;
    int                     listen_fd = -1;
    uint32_t                sub_instance = 0;

    int bind_listen_() {
        int fd = ::socket(AF_TIPC, SOCK_SEQPACKET, 0);
        if (fd < 0) { std::perror("[log_link] socket"); return -1; }
        struct sockaddr_tipc addr{};
        addr.family   = AF_TIPC;
        addr.addrtype = TIPC_SERVICE_ADDR;
        addr.scope    = TIPC_NODE_SCOPE;
        addr.addr.name.name.type     = kSubType;
        addr.addr.name.name.instance = sub_instance;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::perror("[log_link] bind"); ::close(fd); return -1;
        }
        if (::listen(fd, kBacklog) < 0) {
            std::perror("[log_link] listen"); ::close(fd); return -1;
        }
        return fd;
    }

    // Accept the hub's connection, then recv frames until stop. The hub spills
    // its ring backlog on subscribe, then live records follow. Each frame is
    // [TheiaMsgHeader][LogRecord proto bytes]; we forward the payload verbatim
    // to the sink (the gRPC edge re-parses it).
    void rx_loop() {
        uint8_t buf[kRecvBuf];
        int conn = -1;
        while (running.load()) {
            if (conn < 0) {
                fd_set rfds; FD_ZERO(&rfds); FD_SET(listen_fd, &rfds);
                struct timeval tv{0, 200 * 1000};
                int r = ::select(listen_fd + 1, &rfds, nullptr, nullptr, &tv);
                if (r <= 0) continue;
                conn = ::accept(listen_fd, nullptr, nullptr);
                if (conn < 0) continue;
                std::fprintf(stderr, "[log_link] hub connected\n");
            }
            fd_set rfds; FD_ZERO(&rfds); FD_SET(conn, &rfds);
            struct timeval tv{0, 200 * 1000};
            int r = ::select(conn + 1, &rfds, nullptr, nullptr, &tv);
            if (r <= 0) continue;
            ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0) {                        // hub closed; re-accept
                ::close(conn); conn = -1;
                std::fprintf(stderr, "[log_link] hub disconnected\n");
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

LogLink::LogLink() : impl_(new Impl()) {}
LogLink::~LogLink() { stop(); delete impl_; }

LogLink& LogLink::instance() {
    static LogLink s;
    return s;
}

void LogLink::set_sink(LogSink sink) { impl_->sink = std::move(sink); }

bool LogLink::start(int connect_timeout_ms) {
    if (impl_->running.load()) return true;

    impl_->sub_instance =
        static_cast<uint32_t>(::getpid()) & 0xFFFFu;

    impl_->listen_fd = impl_->bind_listen_();
    if (impl_->listen_fd < 0) return false;

    impl_->running.store(true);
    impl_->rx_thread = std::thread([this] { impl_->rx_loop(); });

    // gen_call LogSubscribeReq to LogDaemon. Reply is LogEmpty (just an ack).
    if (!impl_->ctl.connect(connect_timeout_ms)) {
        std::fprintf(stderr,
            "[log_link] LogDaemon (0x%08x/0) unreachable; no log egress\n",
            kLogDaemonType);
        stop();
        return false;
    }
    impl_->reply_mux.watch_remote_ref(impl_->ctl);
    impl_->reply_mux.start();

    system_services_log_LogSubscribeReq req =
        system_services_log_LogSubscribeReq_init_zero;
    req.sub_type     = kSubType;
    req.sub_instance = impl_->sub_instance;
    req.level_min    = system_services_log_LogLevel_LogLevel_VERBOSE;  // all
    // tag_filter left empty (all tags); the fine filter is client-side.

    auto result = theia::runtime::call<system_services_log_LogEmpty>(
        impl_->ctl, req, /*act=*/0, connect_timeout_ms);
    if (result.tag != theia::runtime::CallTag::Reply) {
        std::fprintf(stderr,
            "[log_link] LogSubscribeReq to LogDaemon failed (tag=%d)\n",
            static_cast<int>(result.tag));
        stop();
        return false;
    }
    std::fprintf(stderr,
        "[log_link] subscribed to log hub (sub 0x%08x/%u)\n",
        kSubType, impl_->sub_instance);
    return true;
}

void LogLink::stop() {
    if (!impl_->running.exchange(false)) {
        if (impl_->listen_fd >= 0) { ::close(impl_->listen_fd); impl_->listen_fd = -1; }
        return;
    }
    if (impl_->rx_thread.joinable()) impl_->rx_thread.join();
    impl_->reply_mux.stop();
    if (impl_->listen_fd >= 0) { ::close(impl_->listen_fd); impl_->listen_fd = -1; }
}

bool LogLink::running() const { return impl_->running.load(); }

}  // namespace services_com

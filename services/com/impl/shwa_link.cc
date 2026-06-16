// shwa_link implementation — SOCK_DGRAM receiver bound to SHWA's AccelTelemetry
// egress name + a recv thread that hands each sample's raw bytes to the sink.
//
// SHWA's AccelSubmitter (services/shwa, mirror of the runtime TraceSubmitter)
// sendto's a GEN_CAST datagram [TheiaMsgHeader|AccelSample] to a well-known
// TIPC service NAME. com binds that name (DGRAM) and recvs — connectionless, so
// no Subscribe RPC and no accept(); the socket survives SHWA (re)starting. The
// nanopb header type stays here; the sink takes RAW bytes so the libprotobuf
// gRPC edge (ComGrpcProxy) never meets the nanopb shwa headers. Mirror of
// log_link.cc, minus the LogSubscribeReq call (broadcast egress, not pull-sub).

#include "impl/shwa_link.hpp"

#include "RemoteCodec.hh"      // hash_msg_type_ (kRecordServiceId)
#include "TheiaMsgHeader.hh"

#include <sys/socket.h>
#include <sys/select.h>
#include <linux/tipc.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

namespace services_com {

namespace {

// SHWA's AccelTelemetry egress channel — the SAME well-known name SHWA's
// AccelSubmitter sendto's (services/shwa ShwaDaemon_handlers.cc kEgressTipcType).
// com BINDS this name (DGRAM) and receives the broadcast casts.
constexpr uint32_t kEgressType     = 0x8001001Au;
constexpr uint32_t kEgressInstance = 0u;

// The service_id SHWA stamps on each AccelSample cast — the SAME djb2_low16 the
// AccelSubmitter computes from the nanopb C type name. Derived here so it can
// never drift from the sender.
const uint16_t kRecordServiceId =
    ::theia::runtime::hash_msg_type_("system_services_shwa_AccelSample");

constexpr int kRecvBuf = 512;   // 24B header + AccelSample (~120B) — small.

}  // namespace

struct ShwaLink::Impl {
    AccelSink         sink;
    std::atomic<bool> running{false};
    std::thread       rx_thread;
    int               fd = -1;

    int bind_egress_() {
        int s = ::socket(AF_TIPC, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (s < 0) { std::perror("[shwa_link] socket"); return -1; }
        struct sockaddr_tipc addr{};
        addr.family   = AF_TIPC;
        addr.addrtype = TIPC_SERVICE_ADDR;
        addr.scope    = TIPC_NODE_SCOPE;
        addr.addr.name.name.type     = kEgressType;
        addr.addr.name.name.instance = kEgressInstance;
        if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::perror("[shwa_link] bind"); ::close(s); return -1;
        }
        return s;
    }

    // Recv AccelSample datagrams until stop. Each is [TheiaMsgHeader][AccelSample
    // proto bytes]; forward the payload verbatim to the sink (the gRPC edge
    // re-parses it into a libprotobuf AccelSample).
    void rx_loop() {
        uint8_t buf[kRecvBuf];
        while (running.load()) {
            fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
            struct timeval tv{0, 200 * 1000};
            int r = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
            if (r <= 0) continue;
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
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
    }
};

ShwaLink::ShwaLink() : impl_(new Impl()) {}
ShwaLink::~ShwaLink() { stop(); delete impl_; }

ShwaLink& ShwaLink::instance() {
    static ShwaLink s;
    return s;
}

void ShwaLink::set_sink(AccelSink sink) { impl_->sink = std::move(sink); }

bool ShwaLink::start() {
    if (impl_->running.load()) return true;
    impl_->fd = impl_->bind_egress_();
    if (impl_->fd < 0) return false;
    impl_->running.store(true);
    impl_->rx_thread = std::thread([this] { impl_->rx_loop(); });
    std::fprintf(stderr,
        "[shwa_link] bound AccelTelemetry egress (0x%08x/%u)\n",
        kEgressType, kEgressInstance);
    return true;
}

void ShwaLink::stop() {
    if (!impl_->running.exchange(false)) {
        if (impl_->fd >= 0) { ::close(impl_->fd); impl_->fd = -1; }
        return;
    }
    if (impl_->rx_thread.joinable()) impl_->rx_thread.join();
    if (impl_->fd >= 0) { ::close(impl_->fd); impl_->fd = -1; }
}

bool ShwaLink::running() const { return impl_->running.load(); }

}  // namespace services_com

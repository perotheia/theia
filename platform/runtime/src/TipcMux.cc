#include "TipcMux.hh"

#include "Logger.hh"   // process_logger() — gate the bind/accept chatter at DEBUG

#include <sys/epoll.h>
#include <sys/socket.h>
#include <linux/tipc.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace theia {
namespace runtime {

static constexpr int kBacklog   = 16;
static constexpr int kMaxEvents = 32;
static constexpr int kRecvBuf   = 4096;

TipcMux::TipcMux() {
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0) std::perror("[TipcMux] epoll_create1");
}

TipcMux::~TipcMux() {
    stop();
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
    for (auto& b : bindings_) {
        if (b->listen_fd >= 0) ::close(b->listen_fd);
    }
}

int TipcMux::bind_listen_(uint32_t type, uint32_t instance) {
    int fd = ::socket(AF_TIPC, SOCK_SEQPACKET, 0);
    if (fd < 0) { std::perror("[TipcMux] socket"); return -1; }
    struct sockaddr_tipc addr{};
    addr.family   = AF_TIPC;
    addr.addrtype = TIPC_SERVICE_ADDR;
    addr.scope    = TIPC_NODE_SCOPE;
    addr.addr.name.name.type     = type;
    addr.addr.name.name.instance = instance;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("[TipcMux] bind");
        ::close(fd); return -1;
    }
    if (::listen(fd, kBacklog) < 0) {
        std::perror("[TipcMux] listen");
        ::close(fd); return -1;
    }
    {
        char buf[80];
        std::snprintf(buf, sizeof(buf),
            "[TipcMux] bound {0x%08X, %u} on fd=%d", type, instance, fd);
        process_logger().debug(buf);
    }
    return fd;
}

void TipcMux::add_to_epoll_(int fd, uint32_t event_mask) {
    struct epoll_event ev{};
    ev.events  = event_mask;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        std::perror("[TipcMux] epoll_ctl ADD");
    }
}

NodeBinding* TipcMux::bind_node(GenServerBase& node,
                                  uint32_t tipc_type, uint32_t tipc_instance) {
    int fd = bind_listen_(tipc_type, tipc_instance);
    if (fd < 0) return nullptr;
    auto b = std::unique_ptr<NodeBinding>(new NodeBinding{
        &node, fd, tipc_type, tipc_instance, {}});
    auto* raw = b.get();
    std::lock_guard<std::mutex> lk(mu_);
    add_to_epoll_(fd, EPOLLIN);
    listen_fd_to_binding_[fd] = raw;
    bindings_.push_back(std::move(b));
    return raw;
}

NodeBinding* TipcMux::bind_listener(uint32_t tipc_type,
                                    uint32_t tipc_instance) {
    // Same as bind_node but with node==nullptr — the epoll loop routes by fd
    // and dispatches via entries[service_id], which the inline path fills; it
    // never dereferences binding->node. Only register_cast_inline is valid here.
    int fd = bind_listen_(tipc_type, tipc_instance);
    if (fd < 0) return nullptr;
    auto b = std::unique_ptr<NodeBinding>(new NodeBinding{
        nullptr, fd, tipc_type, tipc_instance, {}});
    auto* raw = b.get();
    std::lock_guard<std::mutex> lk(mu_);
    add_to_epoll_(fd, EPOLLIN);
    listen_fd_to_binding_[fd] = raw;
    bindings_.push_back(std::move(b));
    return raw;
}

NodeBinding* TipcMux::binding_for(uint32_t tipc_type, uint32_t tipc_instance) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& b : bindings_) {
        if (b->tipc_type == tipc_type && b->tipc_instance == tipc_instance)
            return b.get();
    }
    return nullptr;
}

void TipcMux::watch_fd_for_replies_(
    int fd,
    std::function<void(uint32_t, const uint8_t*, uint16_t)> sink) {
    std::lock_guard<std::mutex> lk(mu_);
    add_to_epoll_(fd, EPOLLIN);
    reply_sinks_[fd] = std::move(sink);
}

void TipcMux::unwatch_reply_fd(int fd) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = reply_sinks_.find(fd);
    if (it == reply_sinks_.end()) return;
    reply_sinks_.erase(it);
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
}

void TipcMux::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this]{ this->loop_(); });
}

void TipcMux::stop() {
    if (!running_.exchange(false)) return;
    // The loop wakes up on epoll timeout (we use a 100ms poll cap).
    if (thread_.joinable()) thread_.join();
}

void TipcMux::loop_() {
    struct epoll_event events[kMaxEvents];
    uint8_t buf[kRecvBuf];
    while (running_.load()) {
        int nev = ::epoll_wait(epoll_fd_, events, kMaxEvents, 100);
        if (nev < 0) {
            if (errno == EINTR) continue;
            std::perror("[TipcMux] epoll_wait");
            break;
        }
        for (int i = 0; i < nev; ++i) {
            int fd = events[i].data.fd;

            // Inbound: new connection on a listen fd.
            NodeBinding* listener = nullptr;
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = listen_fd_to_binding_.find(fd);
                if (it != listen_fd_to_binding_.end()) listener = it->second;
            }
            if (listener) {
                int cfd = ::accept(fd, nullptr, nullptr);
                if (cfd >= 0) {
                    std::lock_guard<std::mutex> lk(mu_);
                    add_to_epoll_(cfd, EPOLLIN);
                    client_fd_to_binding_[cfd] = listener;
                    if (process_logger().level() <= LogLevel::Debug) {
                        char buf[96];
                        std::snprintf(buf, sizeof(buf),
                            "[TipcMux] accept on listen=%d -> client=%d "
                            "(node tipc=0x%08X)", fd, cfd, listener->tipc_type);
                        process_logger().debug(buf);
                    }
                }
                continue;
            }

            // Reply path: outbound RemoteRef expecting CALL_REPLY frames.
            //
            // The sink lambda captures the RemoteRef (`this`). ~RemoteRef runs
            // unwatch_reply_fd() (which takes mu_, erases the sink, EPOLL_DELs
            // the fd) and THEN frees the RemoteRef + closes the fd — on a
            // DIFFERENT thread (the caller's). If we copied the sink out, then
            // released mu_, then invoked it, ~RemoteRef could free `this`
            // between the copy and the call → use-after-free (the per-restart
            // probe-churn crash: SIGSEGV in this loop thread). So we hold mu_
            // ACROSS the whole find→recv→dispatch: unwatch_reply_fd() then
            // can't race the dispatch — it blocks until we're done, and only
            // frees the RemoteRef after. on_reply_ takes only the RemoteRef's
            // OWN pending_mu_ and fulfils a promise (no re-entry into this mux),
            // so holding mu_ here is deadlock-free.
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto sit = reply_sinks_.find(fd);
                if (sit != reply_sinks_.end()) {
                    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                    if (n <= 0) {
                        // Peer closed (EOF n==0) or hard error (ECONNRESET n<0).
                        // Drop the sink + EPOLL_DEL so the dead fd stops firing
                        // (else this loop hot-spins). Do NOT ::close(fd): the
                        // reply fd is the RemoteRef's OWN TipcClient socket —
                        // ~RemoteRef/~TipcClient closes it. Closing it here would
                        // race that close and the fd-number could be reused by a
                        // fresh RemoteRef before ~TipcClient closes it → a
                        // double-close on a live fd. Transient EAGAIN/EINTR retry.
                        bool transient = (n < 0) &&
                            (errno == EAGAIN || errno == EWOULDBLOCK ||
                             errno == EINTR);
                        if (!transient) {
                            reply_sinks_.erase(sit);
                            ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
                        }
                        continue;
                    }
                    if (n >= (ssize_t)sizeof(TheiaMsgHeader)) {
                        TheiaMsgHeader hdr{};
                        std::memcpy(&hdr, buf, sizeof(TheiaMsgHeader));
                        if (hdr.msg_type ==
                                ::theia::runtime::kMsgGenCallReply) {
                            // Clamp proto_len to what we actually recv'd so a
                            // truncated/oversized frame can't read past buf.
                            uint16_t avail = (uint16_t)(n - sizeof(TheiaMsgHeader));
                            uint16_t plen = hdr.proto_len < avail ? hdr.proto_len
                                                                  : avail;
                            sit->second(hdr.rpc.correlation_id,
                                        buf + sizeof(TheiaMsgHeader), plen);
                        }
                    }
                    continue;
                }
            }

            // Inbound: data on an accepted client fd.
            NodeBinding* binding = nullptr;
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = client_fd_to_binding_.find(fd);
                if (it != client_fd_to_binding_.end()) binding = it->second;
            }
            if (!binding) continue;

            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                // Close on EOF (n==0) AND on a hard error (n<0 that isn't a
                // spurious EAGAIN/EWOULDBLOCK/EINTR) — a TIPC peer that
                // disconnects often surfaces as ECONNRESET (n<0), not a clean
                // EOF. Leaving such an fd registered leaks it + epoll re-fires
                // it forever (busy-spin). Only a transient EAGAIN may retry.
                bool transient = (n < 0) &&
                    (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
                if (!transient) {
                    std::lock_guard<std::mutex> lk(mu_);
                    client_fd_to_binding_.erase(fd);
                    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
                    ::close(fd);
                }
                continue;
            }
            if (n < (ssize_t)sizeof(TheiaMsgHeader)) continue;

            TheiaMsgHeader hdr{};
            std::memcpy(&hdr, buf, sizeof(TheiaMsgHeader));
            if (hdr.bus_type != ::theia::runtime::kBusTypeRpc) continue;
            if (hdr.msg_type != ::theia::runtime::kMsgGenCast &&
                hdr.msg_type != ::theia::runtime::kMsgGenCall) continue;

            const uint8_t* payload = buf + sizeof(TheiaMsgHeader);
            (void)payload;
            auto eit = binding->entries.find(hdr.rpc.service_id);
            if (eit == binding->entries.end()) {
                // No registered codec claimed this service_id. The
                // wire-info fall-through (synthesize an InfoMsg, deliver
                // the raw bytes to handle_info(const InfoMsg&, State&)) was
                // removed: cross-node traffic is exclusively typed cast /
                // call with a registered RemoteCodec, and an unrouted frame
                // means the running wiring received a message the netgraph
                // says it should never get — a HARD invariant violation.
                // Log CRITICAL and drop; the missing handler is a codegen /
                // netgraph reconciliation bug, not runtime data to deliver.
                std::fprintf(stderr,
                    "[TipcMux] CRITICAL: unrouted inbound frame "
                    "service_id=0x%04X msg_type=0x%02X len=%u on node "
                    "tipc=0x%08X — no register_cast/register_call claimed "
                    "it. Netgraph consistency compromised; dropping.\n",
                    hdr.rpc.service_id, hdr.msg_type, hdr.proto_len,
                    binding->tipc_type);
                continue;
            }
            // Clamp the wire proto_len to what we actually recv'd into buf so a
            // truncated or lying frame can't make dispatch read past the buffer
            // (kRecvBuf=4096; a SEQPACKET datagram larger than that arrives
            // truncated, but hdr.proto_len still claims the original length).
            uint16_t avail = (uint16_t)(n - sizeof(TheiaMsgHeader));
            uint16_t plen = hdr.proto_len < avail ? hdr.proto_len : avail;
            eit->second.dispatch(payload, plen, fd, hdr.rpc.correlation_id);
        }
    }
}

// ---- process-wide TipcMux accessor (mirrors process_logger/_timers) ------

namespace {
TipcMux*& process_mux_slot() noexcept {
    static TipcMux* slot = nullptr;
    return slot;
}
}  // namespace

void set_process_mux(TipcMux* mux) noexcept { process_mux_slot() = mux; }
TipcMux* process_mux() noexcept { return process_mux_slot(); }

// Free hook (declared in NodeRef.hh) — registers a RemoteRef's reply fd
// with the process mux. No-op when no mux is published (a cast-only
// caller never needs reply demux; a unit test may run without a mux).
void watch_reply_fd(
    int fd,
    std::function<void(uint32_t, const uint8_t*, uint16_t)> sink) {
    if (TipcMux* mux = process_mux()) {
        mux->watch_reply_fd(fd, std::move(sink));
    }
}

// Free hook (declared in NodeRef.hh) — unregister a RemoteRef's reply fd from
// the process mux. Called from ~RemoteRef before the socket closes.
void unwatch_reply_fd(int fd) {
    if (TipcMux* mux = process_mux()) {
        mux->unwatch_reply_fd(fd);
    }
}

}  // namespace runtime
}  // namespace theia

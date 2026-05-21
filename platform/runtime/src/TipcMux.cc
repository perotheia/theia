#include "TipcMux.hh"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <linux/tipc.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace demo {
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
    std::fprintf(stderr,
        "[TipcMux] bound {0x%08X, %u} on fd=%d\n", type, instance, fd);
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

void TipcMux::watch_fd_for_replies_(
    int fd,
    std::function<void(uint32_t, const uint8_t*, uint16_t)> sink) {
    std::lock_guard<std::mutex> lk(mu_);
    add_to_epoll_(fd, EPOLLIN);
    reply_sinks_[fd] = std::move(sink);
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
                    std::fprintf(stderr,
                        "[TipcMux] accept on listen=%d -> client=%d (node tipc=0x%08X)\n",
                        fd, cfd, listener->tipc_type);
                }
                continue;
            }

            // Reply path: outbound RemoteRef expecting CALL_REPLY frames.
            std::function<void(uint32_t, const uint8_t*, uint16_t)> reply_sink;
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = reply_sinks_.find(fd);
                if (it != reply_sinks_.end()) reply_sink = it->second;
            }
            if (reply_sink) {
                ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                if (n < (ssize_t)sizeof(GwMessageHeader)) continue;
                GwMessageHeader hdr{};
                std::memcpy(&hdr, buf, sizeof(GwMessageHeader));
                if (hdr.msg_type == GW_MSG_GEN_CALL_REPLY) {
                    reply_sink(hdr.rpc.correlation_id,
                                buf + sizeof(GwMessageHeader),
                                hdr.proto_len);
                }
                continue;
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
                if (n == 0) {
                    std::lock_guard<std::mutex> lk(mu_);
                    client_fd_to_binding_.erase(fd);
                    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
                    ::close(fd);
                }
                continue;
            }
            if (n < (ssize_t)sizeof(GwMessageHeader)) continue;

            GwMessageHeader hdr{};
            std::memcpy(&hdr, buf, sizeof(GwMessageHeader));
            if (hdr.bus_type != GW_BUS_TYPE_RPC) continue;
            if (hdr.msg_type != GW_MSG_GEN_CAST &&
                hdr.msg_type != GW_MSG_GEN_CALL) continue;

            auto eit = binding->entries.find(hdr.rpc.service_id);
            if (eit == binding->entries.end()) {
                std::fprintf(stderr,
                    "[TipcMux] no handler for service_id=0x%04X on node tipc=0x%08X\n",
                    hdr.rpc.service_id, binding->tipc_type);
                continue;
            }
            const uint8_t* payload = buf + sizeof(GwMessageHeader);
            eit->second.dispatch(payload, hdr.proto_len, fd,
                                  hdr.rpc.correlation_id);
        }
    }
}

}  // namespace runtime
}  // namespace demo

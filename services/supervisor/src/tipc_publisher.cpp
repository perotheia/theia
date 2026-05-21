// TIPC publisher implementation. Skeleton only — fan-out broadcast,
// inbound bytes ignored. See header for design notes.

#include "supervisor/tipc_publisher.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <linux/tipc.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace supervisor {

namespace {

void log_err(const char* fn, const char* msg) {
    std::fprintf(stderr, "tipc_publisher: %s: %s\n", fn, msg);
}

}  // namespace

TipcPublisher::~TipcPublisher() {
    close();
}

bool TipcPublisher::open(uint32_t tipc_type, uint32_t tipc_instance) {
    listen_fd_ = ::socket(AF_TIPC, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        log_err("socket(AF_TIPC)", std::strerror(errno));
        return false;
    }

    // Same binding style as gateway/libs/pero_cmp_lnx/lib/gw/src/gw_tipc_server.cpp:
    // TIPC_ADDR_NAME (a.k.a. TIPC_SERVICE_ADDR) is the right addrtype for a
    // server that wants to accept stream-style SOCK_SEQPACKET clients.
    // TIPC_ADDR_NAMESEQ is for publish/subscribe (a range of names), which
    // we don't want.
    struct sockaddr_tipc addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.family                  = AF_TIPC;
    addr.addrtype                = TIPC_ADDR_NAME;
    addr.addr.name.name.type     = tipc_type;
    addr.addr.name.name.instance = tipc_instance;
    addr.scope                   = TIPC_NODE_SCOPE;

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        log_err("bind", std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::listen(listen_fd_, 8) < 0) {
        log_err("listen", std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        log_err("epoll_create1", std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    struct epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = listen_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) {
        log_err("epoll_ctl(listen_fd)", std::strerror(errno));
    }

    return true;
}

void TipcPublisher::close() {
    for (int fd : clients_) {
        ::close(fd);
    }
    clients_.clear();
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (epoll_fd_  >= 0) ::close(epoll_fd_);
    listen_fd_ = -1;
    epoll_fd_  = -1;
}

void TipcPublisher::publish(uint16_t type_tag, const std::string& serialized) {
    if (clients_.empty()) return;

    // Frame: 2-byte big-endian type tag followed by the protobuf payload.
    // SOCK_SEQPACKET preserves boundaries: one send() = one recv() on
    // the other side.
    std::string buf;
    buf.resize(2 + serialized.size());
    buf[0] = static_cast<char>((type_tag >> 8) & 0xff);
    buf[1] = static_cast<char>(type_tag & 0xff);
    std::memcpy(&buf[2], serialized.data(), serialized.size());

    std::vector<int> dead;
    for (int fd : clients_) {
        ssize_t n = ::send(fd, buf.data(), buf.size(), MSG_NOSIGNAL);
        if (n < 0) {
            dead.push_back(fd);
        }
    }
    for (int fd : dead) drop_client(fd);
}

void TipcPublisher::poll() {
    if (epoll_fd_ < 0) return;
    struct epoll_event events[16];
    int n = ::epoll_wait(epoll_fd_, events, 16, 0);
    if (n <= 0) return;

    for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;
        if (fd == listen_fd_) {
            accept_clients();
            continue;
        }
        // Skeleton: drain whatever the client sent and discard.
        // Once the control RPC lands, decode here and dispatch.
        char buf[4096];
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) {
            drop_client(fd);
            continue;
        }
        // ignore the bytes for now
    }
}

void TipcPublisher::accept_clients() {
    for (;;) {
        int cfd = ::accept(listen_fd_, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            log_err("accept", std::strerror(errno));
            break;
        }
        struct epoll_event ev{};
        ev.events  = EPOLLIN | EPOLLRDHUP;
        ev.data.fd = cfd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, cfd, &ev) < 0) {
            log_err("epoll_ctl(client)", std::strerror(errno));
            ::close(cfd);
            continue;
        }
        clients_.push_back(cfd);
        std::fprintf(stderr, "tipc_publisher: client connected (fd=%d, total=%zu)\n",
                     cfd, clients_.size());
    }
}

void TipcPublisher::drop_client(int fd) {
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), fd), clients_.end());
    std::fprintf(stderr, "tipc_publisher: client disconnected (fd=%d, remaining=%zu)\n",
                 fd, clients_.size());
}

}  // namespace supervisor

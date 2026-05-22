// TCP publisher implementation. See header for design notes.

#include "supervisor/tcp_publisher.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace supervisor {

namespace {

void log_err(const char* fn, const char* msg) {
    std::fprintf(stderr, "tcp_publisher: %s: %s\n", fn, msg);
}

// Big-endian writes (htonl/htons in a portable buffer-write form).
void write_be32(char* dst, uint32_t v) {
    dst[0] = static_cast<char>((v >> 24) & 0xff);
    dst[1] = static_cast<char>((v >> 16) & 0xff);
    dst[2] = static_cast<char>((v >> 8) & 0xff);
    dst[3] = static_cast<char>(v & 0xff);
}
void write_be16(char* dst, uint16_t v) {
    dst[0] = static_cast<char>((v >> 8) & 0xff);
    dst[1] = static_cast<char>(v & 0xff);
}

}  // namespace

TcpPublisher::~TcpPublisher() {
    close();
}

bool TcpPublisher::open(uint16_t port) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        log_err("socket(AF_INET)", std::strerror(errno));
        return false;
    }

    int yes = 1;
    if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        log_err("setsockopt(SO_REUSEADDR)", std::strerror(errno));
        // non-fatal
    }

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

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

    std::fprintf(stderr, "tcp_publisher: listening on 0.0.0.0:%u\n", port);
    return true;
}

void TcpPublisher::close() {
    for (int fd : clients_) {
        ::close(fd);
    }
    clients_.clear();
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (epoll_fd_  >= 0) ::close(epoll_fd_);
    listen_fd_ = -1;
    epoll_fd_  = -1;
}

void TcpPublisher::publish(uint16_t type_tag, const std::string& serialized) {
    if (clients_.empty()) return;

    // Frame: uint32_be(payload_len) + uint16_be(type_tag) + serialized.
    // payload_len includes the 2-byte tag so the receiver can skip
    // unknown frames by reading exactly payload_len bytes after the
    // length header.
    const uint32_t payload_len = 2 + static_cast<uint32_t>(serialized.size());
    std::string buf;
    buf.resize(4 + payload_len);
    write_be32(&buf[0], payload_len);
    write_be16(&buf[4], type_tag);
    std::memcpy(&buf[6], serialized.data(), serialized.size());

    std::vector<int> dead;
    for (int fd : clients_) {
        // TCP is a byte stream — a partial send is possible but for the
        // small frame sizes we publish (KB-scale, max) the kernel
        // buffers the whole thing in one go on a fresh socket. Loop on
        // EINTR; treat any other error as "drop this client".
        const char* p = buf.data();
        size_t      remaining = buf.size();
        bool        failed = false;
        while (remaining) {
            ssize_t n = ::send(fd, p, remaining, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) continue;
                failed = true;
                break;
            }
            p += n;
            remaining -= static_cast<size_t>(n);
        }
        if (failed) dead.push_back(fd);
    }
    for (int fd : dead) drop_client(fd);
}

void TcpPublisher::poll() {
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
        // Drain whatever the client sent and discard. When the control
        // RPC arrives, parse here and dispatch.
        char buf[4096];
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        if (r <= 0) {
            drop_client(fd);
            continue;
        }
        // ignore the bytes
    }
}

void TcpPublisher::accept_clients() {
    for (;;) {
        int cfd = ::accept4(listen_fd_, nullptr, nullptr,
                             SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            log_err("accept", std::strerror(errno));
            break;
        }
        // Disable Nagle for the GUI link — frames are small and we want
        // them delivered immediately rather than coalesced.
        int yes = 1;
        ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

        struct epoll_event ev{};
        ev.events  = EPOLLIN | EPOLLRDHUP;
        ev.data.fd = cfd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, cfd, &ev) < 0) {
            log_err("epoll_ctl(client)", std::strerror(errno));
            ::close(cfd);
            continue;
        }
        clients_.push_back(cfd);
        std::fprintf(stderr, "tcp_publisher: client connected (fd=%d, total=%zu)\n",
                     cfd, clients_.size());
    }
}

void TcpPublisher::drop_client(int fd) {
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), fd), clients_.end());
    std::fprintf(stderr, "tcp_publisher: client disconnected (fd=%d, remaining=%zu)\n",
                 fd, clients_.size());
}

}  // namespace supervisor

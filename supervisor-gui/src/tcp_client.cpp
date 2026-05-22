#include "sup_gui/tcp_client.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace sup_gui {

namespace {

uint32_t read_be32(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
            static_cast<uint32_t>(p[3]);
}
uint16_t read_be16(const unsigned char* p) {
    return (static_cast<uint16_t>(p[0]) << 8) |
            static_cast<uint16_t>(p[1]);
}

}  // namespace

TcpClient::TcpClient(std::string machine_name,
                     std::string host,
                     uint16_t port,
                     FrameCallback on_frame)
    : machine_name_(std::move(machine_name)),
      host_(std::move(host)),
      port_(port),
      callback_(std::move(on_frame)) {}

TcpClient::~TcpClient() {
    stop();
}

void TcpClient::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
}

void TcpClient::stop() {
    if (!running_.exchange(false)) return;
    // Closing the socket triggers recv() to return; the thread loop sees
    // running_ == false and exits.
    disconnect_socket();
    if (thread_.joinable()) thread_.join();
}

bool TcpClient::connect_socket() {
    // Resolve host (IPv4 only — supervisor binds AF_INET).
    struct addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portbuf[8];
    std::snprintf(portbuf, sizeof(portbuf), "%u", port_);
    struct addrinfo* res = nullptr;
    int gai = ::getaddrinfo(host_.c_str(), portbuf, &hints, &res);
    if (gai != 0 || res == nullptr) {
        // Suppress noise on every retry — only log first time per cycle.
        return false;
    }

    fd_ = ::socket(res->ai_family, res->ai_socktype | SOCK_CLOEXEC,
                    res->ai_protocol);
    if (fd_ < 0) {
        ::freeaddrinfo(res);
        return false;
    }

    if (::connect(fd_, res->ai_addr, res->ai_addrlen) < 0) {
        ::close(fd_);
        fd_ = -1;
        ::freeaddrinfo(res);
        return false;
    }
    ::freeaddrinfo(res);

    int yes = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    connected_.store(true);
    std::fprintf(stderr, "tcp_client[%s]: connected (fd=%d, %s:%u)\n",
                 machine_name_.c_str(), fd_, host_.c_str(), port_);
    return true;
}

void TcpClient::disconnect_socket() {
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
    connected_.store(false);
}

bool TcpClient::read_exact(void* buf, size_t n) {
    auto* p = static_cast<unsigned char*>(buf);
    while (n) {
        ssize_t r = ::recv(fd_, p, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

void TcpClient::run() {
    constexpr size_t kMaxPayload = 16 * 1024 * 1024;  // 16 MiB sanity cap

    while (running_.load()) {
        if (!connect_socket()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        while (running_.load()) {
            // Read header: 4-byte BE length.
            unsigned char hdr[4];
            if (!read_exact(hdr, sizeof(hdr))) break;
            uint32_t payload_len = read_be32(hdr);
            if (payload_len < 2 || payload_len > kMaxPayload) {
                std::fprintf(stderr,
                    "tcp_client[%s]: bogus length %u — dropping\n",
                    machine_name_.c_str(), payload_len);
                break;
            }

            // Read payload: tag (2 bytes) + protobuf.
            std::string buf;
            buf.resize(payload_len);
            // C++14: std::string::data() is const. Use &buf[0] for the
            // mutable handle we need for read(2). buf.resize() guarantees
            // contiguous storage.
            if (!read_exact(&buf[0], payload_len)) break;

            uint16_t tag = read_be16(
                reinterpret_cast<const unsigned char*>(buf.data()));
            std::string payload(buf.data() + 2, payload_len - 2);

            if (callback_) callback_(machine_name_, tag, std::move(payload));
        }

        disconnect_socket();
        if (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    disconnect_socket();
}

}  // namespace sup_gui

#include "sup_gui/tipc_client.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <linux/tipc.h>
#include <sys/socket.h>
#include <unistd.h>

namespace sup_gui {

TipcClient::TipcClient(uint32_t tipc_type, uint32_t tipc_instance,
                       FrameCallback on_frame)
    : tipc_type_(tipc_type),
      tipc_instance_(tipc_instance),
      callback_(std::move(on_frame)) {}

TipcClient::~TipcClient() {
    stop();
}

void TipcClient::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] { run(); });
}

void TipcClient::stop() {
    if (!running_.exchange(false)) return;
    // Closing the socket triggers recv() to return; the thread loop sees
    // running_ == false and exits.
    disconnect_socket();
    if (thread_.joinable()) thread_.join();
}

bool TipcClient::connect_socket() {
    fd_ = ::socket(AF_TIPC, SOCK_SEQPACKET, 0);
    if (fd_ < 0) {
        std::fprintf(stderr, "tipc_client: socket: %s\n", std::strerror(errno));
        return false;
    }
    struct sockaddr_tipc addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.family             = AF_TIPC;
    addr.addrtype           = TIPC_ADDR_NAME;
    addr.addr.name.name.type     = tipc_type_;
    addr.addr.name.name.instance = tipc_instance_;
    addr.scope              = TIPC_NODE_SCOPE;

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    connected_.store(true);
    std::fprintf(stderr, "tipc_client: connected (fd=%d, type=0x%x, inst=%u)\n",
                 fd_, tipc_type_, tipc_instance_);
    return true;
}

void TipcClient::disconnect_socket() {
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
    connected_.store(false);
}

void TipcClient::run() {
    while (running_.load()) {
        if (!connect_socket()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        char buf[65536];
        while (running_.load()) {
            ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && errno == ENOTCONN) {
                // TIPC SOCK_SEQPACKET quirk: connect() returns 0 before
                // the server has accept()ed; until then, recv() returns
                // ENOTCONN. Back off briefly and try again — the
                // supervisor accepts on its next poll() tick.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (n <= 0) {
                std::fprintf(stderr, "tipc_client: recv: %s\n",
                             n == 0 ? "peer closed" : std::strerror(errno));
                break;
            }
            if (n < 2) continue;  // malformed; need at least the type tag

            uint16_t tag = (static_cast<uint8_t>(buf[0]) << 8) |
                            static_cast<uint8_t>(buf[1]);
            std::string payload(buf + 2, static_cast<size_t>(n) - 2);
            if (callback_) callback_(tag, std::move(payload));
        }

        disconnect_socket();
        if (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    disconnect_socket();
}

}  // namespace sup_gui

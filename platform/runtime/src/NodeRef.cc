// TipcClient: connect-with-retry + raw frame send. See NodeRef.hh.

#include "NodeRef.hh"

#include <sys/socket.h>
#include <linux/tipc.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <thread>

namespace demo {
namespace runtime {

TipcClient::~TipcClient() { disconnect(); }

bool TipcClient::connect(uint32_t tipc_type, uint32_t tipc_instance,
                          int total_timeout_ms, int retry_ms) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(total_timeout_ms);
    while (true) {
        fd_ = ::socket(AF_TIPC, SOCK_SEQPACKET, 0);
        if (fd_ < 0) {
            std::perror("[TipcClient] socket");
            return false;
        }
        struct sockaddr_tipc addr{};
        addr.family   = AF_TIPC;
        addr.addrtype = TIPC_SERVICE_ADDR;
        addr.scope    = TIPC_NODE_SCOPE;
        addr.addr.name.name.type     = tipc_type;
        addr.addr.name.name.instance = tipc_instance;

        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr),
                       sizeof(addr)) == 0) {
            return true;
        }
        ::close(fd_);
        fd_ = -1;
        if (std::chrono::steady_clock::now() > deadline) {
            std::fprintf(stderr,
                "[TipcClient] connect to {0x%08X, %u} failed after %dms\n",
                tipc_type, tipc_instance, total_timeout_ms);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(retry_ms));
    }
}

void TipcClient::disconnect() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

bool TipcClient::send_frame(const GwMessageHeader& hdr,
                             const uint8_t* payload, uint16_t proto_len) {
    if (fd_ < 0) return false;
    uint8_t buf[sizeof(GwMessageHeader) + 256];
    size_t total = sizeof(GwMessageHeader) + proto_len;
    if (total > sizeof(buf)) return false;
    std::memcpy(buf, &hdr, sizeof(GwMessageHeader));
    if (proto_len > 0 && payload) {
        std::memcpy(buf + sizeof(GwMessageHeader), payload, proto_len);
    }
    ssize_t sent = ::send(fd_, buf, total, MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(total);
}

}  // namespace runtime
}  // namespace demo

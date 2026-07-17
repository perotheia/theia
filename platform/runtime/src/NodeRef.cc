// TipcClient: connect-with-retry + raw frame send. See NodeRef.hh.

#include "NodeRef.hh"

#include <sys/socket.h>
#include <linux/tipc.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <thread>

namespace theia {
namespace runtime {

TipcClient::~TipcClient() { disconnect(); }

bool TipcClient::connect(uint32_t tipc_type, uint32_t tipc_instance,
                          int total_timeout_ms, int retry_ms) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(total_timeout_ms);
    while (true) {
        // NON-BLOCKING connect. A BLOCKING ::connect() to an absent TIPC
        // service hangs in-kernel for TIPC's own ~8s timeout, which IGNORES
        // total_timeout_ms (the deadline below is only checked BETWEEN
        // retries — it can't interrupt a syscall already blocked). So we set
        // SOCK_NONBLOCK, get EINPROGRESS, and poll() the fd for the REMAINING
        // budget; only then is total_timeout_ms actually honored. On success
        // we clear O_NONBLOCK so send/recv stay blocking as before.
        fd_ = ::socket(AF_TIPC, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
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

        int rc = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr),
                            sizeof(addr));
        bool connected = false;
        if (rc == 0) {
            connected = true;                       // immediate success (rare)
        } else if (errno == EINPROGRESS) {
            // Wait up to the remaining budget for the connect to complete.
            auto now = std::chrono::steady_clock::now();
            int remain_ms = (now >= deadline) ? 0 : static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now).count());
            struct pollfd pfd{fd_, POLLOUT, 0};
            int pr = ::poll(&pfd, 1, remain_ms);
            if (pr > 0 && (pfd.revents & POLLOUT)) {
                int err = 0; socklen_t len = sizeof(err);
                if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) == 0 &&
                    err == 0) {
                    connected = true;
                }
            }
            // pr == 0 → timed out; pr < 0 → poll error; SO_ERROR set → refused.
        }

        if (connected) {
            // Restore blocking mode for the normal send/recv path.
            int fl = ::fcntl(fd_, F_GETFL, 0);
            if (fl >= 0) ::fcntl(fd_, F_SETFL, fl & ~O_NONBLOCK);
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

bool TipcClient::send_frame(const TheiaMsgHeader& hdr,
                             const uint8_t* payload, uint16_t proto_len) {
    if (fd_ < 0) return false;
    uint8_t buf[sizeof(TheiaMsgHeader) + kMaxCastPayload];
    size_t total = sizeof(TheiaMsgHeader) + proto_len;
    if (total > sizeof(buf)) return false;
    std::memcpy(buf, &hdr, sizeof(TheiaMsgHeader));
    if (proto_len > 0 && payload) {
        std::memcpy(buf + sizeof(TheiaMsgHeader), payload, proto_len);
    }
    ssize_t sent = ::send(fd_, buf, total, MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(total);
}

}  // namespace runtime
}  // namespace theia

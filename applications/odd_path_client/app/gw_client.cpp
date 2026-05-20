#include "gw_client.h"

#include <sys/socket.h>
#include <linux/tipc.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <stdexcept>

bool GwClient::connect()
{
    fd_ = ::socket(AF_TIPC, SOCK_SEQPACKET, 0);
    if (fd_ < 0) {
        std::perror("[GwClient] socket");
        return false;
    }

    struct sockaddr_tipc addr{};
    addr.family              = AF_TIPC;
    addr.addrtype            = TIPC_SERVICE_ADDR;
    addr.scope               = TIPC_NODE_SCOPE;
    addr.addr.name.name.type     = TIPC_GW_TYPE;
    addr.addr.name.name.instance = TIPC_GW_INSTANCE;

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) < 0) {
        std::perror("[GwClient] connect");
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    return true;
}

void GwClient::disconnect()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

ssize_t GwClient::recv_signal(GwMessageHeader* hdr_out,
                               uint8_t* proto_buf, size_t proto_buf_size,
                               int timeout_ms)
{
    if (fd_ < 0) return -1;

    struct pollfd pfd{};
    pfd.fd     = fd_;
    pfd.events = POLLIN;

    int ret = ::poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
        if (errno == EINTR) return 0;
        std::perror("[GwClient] poll");
        return -1;
    }
    if (ret == 0) return 0;  /* timeout */

    if (pfd.revents & (POLLHUP | POLLERR)) {
        std::fprintf(stderr, "[GwClient] Connection closed by server\n");
        disconnect();
        return -1;
    }

    uint8_t buf[sizeof(GwMessageHeader) + 256];
    ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
    if (n <= 0) {
        if (n == 0) std::fprintf(stderr, "[GwClient] GW disconnected\n");
        else        std::perror("[GwClient] recv");
        disconnect();
        return -1;
    }

    if (n < static_cast<ssize_t>(sizeof(GwMessageHeader))) return 0;

    GwMessageHeader hdr_local{};
    std::memcpy(&hdr_local, buf, sizeof(GwMessageHeader));

    uint16_t plen = hdr_local.proto_len;
    const uint8_t* pdata = buf + sizeof(GwMessageHeader);

    /* Intercept RPC responses: fulfill the matching promise and drop
     * this frame from the caller's view (return 0 = "no signal"). */
    if (hdr_local.msg_type == GW_MSG_RPC_RESPONSE) {
        fulfill_response(hdr_local, pdata, plen);
        return 0;
    }

    if (hdr_out) std::memcpy(hdr_out, &hdr_local, sizeof(GwMessageHeader));
    if (plen > 0 && proto_buf && proto_buf_size > 0) {
        size_t copy_sz = plen < proto_buf_size ? plen : proto_buf_size;
        std::memcpy(proto_buf, pdata, copy_sz);
    }

    return (ssize_t)plen;
}

bool GwClient::send_tx_request(uint32_t can_id, uint8_t channel_idx,
                                uint8_t bus_type,
                                const uint8_t* proto_data, uint16_t proto_len)
{
    if (fd_ < 0) return false;

    uint8_t buf[sizeof(GwMessageHeader) + 256];
    GwMessageHeader hdr{};
    hdr.bus_type          = bus_type;           /* GW_BUS_TYPE_CAN | GW_BUS_TYPE_FLEXRAY */
    hdr.msg_type          = GW_MSG_TX_REQUEST;
    hdr.proto_len         = proto_len;
    hdr.timestamp_ns      = 0u;                 /* client TX: no capture timestamp */
    /* Fill bus-specific union based on bus_type */
    if (bus_type == GW_BUS_TYPE_CAN) {
        hdr.can.can_id      = can_id;
        hdr.can.channel_idx = channel_idx;
        hdr.can.bus_id      = 0u;               /* caller may set via bus_id param */
        hdr.can.dlc         = 0u;
        hdr.can.flags       = 0u;
    } else {
        hdr.flexray.slot_id     = (uint16_t)can_id;
        hdr.flexray.channel_idx = channel_idx;
        hdr.flexray.bus_id      = 0u;
        hdr.flexray.cycle       = 0u;
        hdr.flexray.pdu_offset  = 0u;
    }

    std::memcpy(buf, &hdr, sizeof(GwMessageHeader));
    size_t total = sizeof(GwMessageHeader);
    if (proto_len > 0 && proto_data) {
        size_t copy_sz = proto_len < 256u ? proto_len : 256u;
        std::memcpy(buf + sizeof(GwMessageHeader), proto_data, copy_sz);
        total += copy_sz;
    }

    ssize_t sent = ::send(fd_, buf, total, MSG_NOSIGNAL);
    if (sent < 0) {
        std::perror("[GwClient] send");
        return false;
    }
    return sent == static_cast<ssize_t>(total);
}

std::future<std::vector<uint8_t>> GwClient::send_request(
    uint16_t service_id, uint16_t method_id,
    const uint8_t* req_data, uint16_t req_len)
{
    std::promise<std::vector<uint8_t>> p;
    auto fut = p.get_future();

    if (fd_ < 0) {
        p.set_exception(std::make_exception_ptr(
            std::runtime_error("GwClient not connected")));
        return fut;
    }

    uint32_t corr;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        corr = next_correlation_id_++;
        pending_.emplace(corr, std::move(p));
    }

    uint8_t buf[sizeof(GwMessageHeader) + 256];
    GwMessageHeader hdr{};
    hdr.bus_type           = GW_BUS_TYPE_RPC;
    hdr.msg_type           = GW_MSG_RPC_REQUEST;
    hdr.proto_len          = req_len;
    hdr.rpc.service_id     = service_id;
    hdr.rpc.method_id      = method_id;
    hdr.rpc.correlation_id = corr;

    std::memcpy(buf, &hdr, sizeof(GwMessageHeader));
    size_t total = sizeof(GwMessageHeader);
    if (req_len > 0 && req_data) {
        size_t copy_sz = req_len < 256u ? req_len : 256u;
        std::memcpy(buf + sizeof(GwMessageHeader), req_data, copy_sz);
        total += copy_sz;
    }

    ssize_t sent = ::send(fd_, buf, total, MSG_NOSIGNAL);
    if (sent != static_cast<ssize_t>(total)) {
        std::perror("[GwClient] send_request send");
        std::lock_guard<std::mutex> lk(pending_mu_);
        auto it = pending_.find(corr);
        if (it != pending_.end()) {
            it->second.set_exception(std::make_exception_ptr(
                std::runtime_error("send failed")));
            pending_.erase(it);
        }
    }
    return fut;
}

bool GwClient::pump_response(int timeout_ms)
{
    if (fd_ < 0) return false;

    /* In pcap-replay mode the gateway floods SIGNAL_UPDATE frames at full
     * speed, so a single recv() almost always lands on a signal, not on
     * our RPC response. Loop until either (a) a RESPONSE is demultiplexed,
     * (b) the total wall-clock budget expires, or (c) the socket closes.
     * Each iteration uses a shrinking timeout so we honour the cap. */
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(timeout_ms);
    while (true) {
        auto now = std::chrono::steady_clock::now();
        int remaining_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count());
        if (remaining_ms <= 0) return false;

        struct pollfd pfd{};
        pfd.fd     = fd_;
        pfd.events = POLLIN;
        int ret = ::poll(&pfd, 1, remaining_ms);
        if (ret <= 0) return false;
        if (pfd.revents & (POLLHUP | POLLERR)) { disconnect(); return false; }

        uint8_t buf[sizeof(GwMessageHeader) + 256];
        ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n < static_cast<ssize_t>(sizeof(GwMessageHeader))) continue;

        GwMessageHeader hdr{};
        std::memcpy(&hdr, buf, sizeof(GwMessageHeader));
        if (hdr.msg_type != GW_MSG_RPC_RESPONSE) {
            /* Signal frame: drop and keep waiting. */
            continue;
        }
        return fulfill_response(hdr, buf + sizeof(GwMessageHeader),
                                 hdr.proto_len);
    }
}

bool GwClient::fulfill_response(const GwMessageHeader& hdr,
                                 const uint8_t* proto, uint16_t proto_len)
{
    std::promise<std::vector<uint8_t>> p;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        auto it = pending_.find(hdr.rpc.correlation_id);
        if (it == pending_.end()) {
            std::fprintf(stderr,
                "[GwClient] orphan RPC response corr=%u — dropping\n",
                hdr.rpc.correlation_id);
            return false;
        }
        p = std::move(it->second);
        pending_.erase(it);
    }
    std::vector<uint8_t> payload(proto, proto + proto_len);
    p.set_value(std::move(payload));
    return true;
}

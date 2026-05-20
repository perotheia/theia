#include "gw_client.h"

#include <sys/socket.h>
#include <linux/tipc.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdio>

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

    std::memcpy(hdr_out, buf, sizeof(GwMessageHeader));

    uint16_t plen = hdr_out->proto_len;
    if (plen > 0 && proto_buf && proto_buf_size > 0) {
        size_t copy_sz = plen < proto_buf_size ? plen : proto_buf_size;
        std::memcpy(proto_buf, buf + sizeof(GwMessageHeader), copy_sz);
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

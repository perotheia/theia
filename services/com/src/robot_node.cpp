// Robot node (#387) — TIPC cast/call client. See robot_node.hpp.

#include "com/robot_node.hpp"

#include <linux/tipc.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <poll.h>

namespace services_com {

namespace {

// Packed mirror of platform's GwMessageHeader (gateway/libs/libgw/
// include/gw_proto.h) — 24 bytes, static_assert-guarded over there.
// com doesn't link libgw, so we replicate the layout. Same trick as
// the supervisor's #386 send_gw_cast_to_tipc_name.
#pragma pack(push, 1)
struct GwHdrWire {
    uint8_t  bus_type;        // GW_BUS_TYPE_RPC
    uint8_t  msg_type;        // GW_MSG_GEN_CAST | GW_MSG_GEN_CALL | _REPLY
    uint16_t proto_len;
    uint64_t timestamp_ns;
    uint16_t service_id;      // djb2_low16 of the nanopb C type name
    uint16_t method_id;       // 0
    uint32_t correlation_id;  // cast: 0; call: caller-assigned
    uint16_t tipc_seq;
    uint8_t  tipc_rsvd[2];
};
#pragma pack(pop)
static_assert(sizeof(GwHdrWire) == 24, "GwMessageHeader is 24 bytes");

constexpr uint8_t kGwBusTypeRpc      = 2u;     // GW_BUS_TYPE_RPC
constexpr uint8_t kGwMsgGenCast      = 0x20u;  // GW_MSG_GEN_CAST
constexpr uint8_t kGwMsgGenCall      = 0x21u;  // GW_MSG_GEN_CALL
constexpr uint8_t kGwMsgGenCallReply = 0x22u;  // GW_MSG_GEN_CALL_REPLY

// Connect a SEQPACKET socket to a TIPC NAME. Returns fd or -1.
int connect_tipc_name(uint32_t type, uint32_t instance) noexcept {
    int fd = ::socket(AF_TIPC, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_tipc addr{};
    addr.family                  = AF_TIPC;
    addr.addrtype                = TIPC_ADDR_NAME;
    addr.addr.name.name.type     = type;
    addr.addr.name.name.instance = instance;
    addr.scope                   = TIPC_NODE_SCOPE;
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool send_framed(int fd, uint8_t gw_msg_type, uint16_t service_id,
                 uint32_t corr, const std::string& payload) noexcept {
    GwHdrWire hdr{};
    hdr.bus_type       = kGwBusTypeRpc;
    hdr.msg_type       = gw_msg_type;
    hdr.proto_len      = static_cast<uint16_t>(payload.size());
    hdr.service_id     = service_id;
    hdr.correlation_id = corr;
    std::string frame(sizeof(GwHdrWire) + payload.size(), '\0');
    std::memcpy(&frame[0], &hdr, sizeof(GwHdrWire));
    if (!payload.empty()) {
        std::memcpy(&frame[sizeof(GwHdrWire)], payload.data(), payload.size());
    }
    ssize_t n = ::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
    return n == static_cast<ssize_t>(frame.size());
}

}  // namespace

uint16_t robot_djb2_low16(const char* s) noexcept {
    uint32_t h = 5381;
    while (*s) { h = (h * 33) + static_cast<unsigned char>(*s++); }
    return static_cast<uint16_t>(h & 0xFFFFu);
}

bool robot_inject_signal(uint32_t tipc_type, uint32_t tipc_instance,
                         const std::string& msg_type,
                         const std::string& payload) noexcept {
    int fd = connect_tipc_name(tipc_type, tipc_instance);
    if (fd < 0) return false;
    bool ok = send_framed(fd, kGwMsgGenCast,
                          robot_djb2_low16(msg_type.c_str()),
                          /*corr=*/0, payload);
    ::close(fd);
    return ok;
}

RobotCallResult robot_call_service(uint32_t tipc_type, uint32_t tipc_instance,
                                   const std::string& req_msg_type,
                                   const std::string& payload,
                                   int timeout_ms) noexcept {
    RobotCallResult r;
    int fd = connect_tipc_name(tipc_type, tipc_instance);
    if (fd < 0) { r.error = "connect failed"; return r; }

    const uint32_t corr = 1;  // single in-flight call per connection
    if (!send_framed(fd, kGwMsgGenCall,
                     robot_djb2_low16(req_msg_type.c_str()), corr, payload)) {
        r.error = "send failed";
        ::close(fd);
        return r;
    }

    // Await the reply on the same socket. The receiver replies on the
    // connection it got the request on (TipcMux register_call), so the
    // reply lands here; correlation_id echoes our `corr`.
    if (timeout_ms <= 0) timeout_ms = 5000;
    struct pollfd pfd{fd, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr <= 0) {
        r.error = (pr == 0) ? "reply timeout" : "poll error";
        ::close(fd);
        return r;
    }

    uint8_t buf[2048];
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    ::close(fd);
    if (n < static_cast<ssize_t>(sizeof(GwHdrWire))) {
        r.error = "short reply frame";
        return r;
    }
    GwHdrWire rh{};
    std::memcpy(&rh, buf, sizeof(GwHdrWire));
    if (rh.msg_type != kGwMsgGenCallReply || rh.correlation_id != corr) {
        r.error = "unexpected reply frame";
        return r;
    }
    const size_t body = static_cast<size_t>(n) - sizeof(GwHdrWire);
    const size_t want = rh.proto_len;
    const size_t take = want <= body ? want : body;
    r.reply_payload.assign(reinterpret_cast<const char*>(buf + sizeof(GwHdrWire)),
                           take);
    r.ok = true;
    return r;
}

}  // namespace services_com

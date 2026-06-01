// TIPC publisher / receiver for the supervisor.
//
// Binds AF_TIPC SOCK_SEQPACKET at the address declared in
// platform/supervisor/system/package.art (TIPC type 0x80020001, instance
// 0). Fans out events / health / snapshots to every connected client
// (services/com bridges these to gRPC for the GUI; in-host actors
// can also subscribe directly).
//
// Wire format: each SEQPACKET datagram is one frame. SEQPACKET
// preserves message boundaries — one send() = one recv().
//
//     uint16_be(type_tag) + protobuf_bytes
//
// type_tag values:
//   0x0001 SupervisionEvent       (sup → consumer)
//   0x0002 HealthBeacon           (sup → consumer)
//   0x0003 TreeSnapshot           (sup → consumer)
//   0x0100 ControlRequest         (consumer → sup) — phase 3
//   0x0101 ControlReply           (sup → consumer) — phase 3
//   0x0200 HeartbeatReport        (node → sup)     — phase 4
//   0x0201 SendTimeoutReport      (node → sup)     — phase 4
//
// Inbound bytes are handed to ``inbound_cb_`` if set; otherwise dropped.
// The supervisor runtime installs a callback during init that routes
// the tag into start_child / restart_child / heartbeat-watchdog / etc.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace supervisor {

class TipcPublisher {
public:
    // Inbound dispatch hook. Args: (client_fd, type_tag, payload_bytes).
    // client_fd is the SOCKET that delivered this frame — the runtime
    // uses it with reply_to() to send ControlReply back to the same
    // peer that issued the ControlRequest.
    using InboundCallback = std::function<void(int /*client_fd*/,
                                               uint16_t /*tag*/,
                                               const std::string& /*payload*/)>;

    TipcPublisher() = default;
    ~TipcPublisher();

    TipcPublisher(const TipcPublisher&)            = delete;
    TipcPublisher& operator=(const TipcPublisher&) = delete;

    bool open(uint32_t tipc_type, uint32_t tipc_instance);
    void close();
    bool is_open() const { return listen_fd_ >= 0; }

    // Install the inbound dispatch callback. Pass an empty std::function
    // to disable (no-op, frames are discarded).
    void set_inbound_callback(InboundCallback cb) {
        inbound_cb_ = std::move(cb);
    }

    // Broadcast one tagged frame to every connected client.
    void publish(uint16_t type_tag, const std::string& serialized);

    // Send to exactly one client_fd (used for ControlReply targeted at
    // the peer that issued the request). Returns true on success.
    // false → fd was closed; drop_client_internal removes it.
    bool reply_to(int client_fd, uint16_t type_tag,
                  const std::string& serialized);

    // Drain pending connect / disconnect / inbound-msg events.
    void poll();

private:
    int              listen_fd_ = -1;
    std::vector<int> clients_;
    int              epoll_fd_  = -1;
    InboundCallback  inbound_cb_;

    void accept_clients();
    void drop_client(int fd);
};

}  // namespace supervisor

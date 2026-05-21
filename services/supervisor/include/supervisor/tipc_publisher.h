// TIPC publisher for the supervisor.
//
// Binds AF_TIPC SOCK_SEQPACKET at the address declared in
// services/supervisor/system/package.art (TIPC type 0x80020001, instance
// 0). Fans out events / health / snapshots to every connected client
// (currently: supervisor-gui).
//
// Wire format is newline-terminated JSON; one document per send.
// SOCK_SEQPACKET preserves message boundaries so each recv() yields
// exactly one JSON document. Format and schema track the messages
// declared in the supervisor .art file.
//
// Skeleton today: pub side only. The control RPC (GetTree, RestartChild,
// etc.) wires in once the GUI exercises it; for now inbound bytes are
// ignored.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace supervisor {

class TipcPublisher {
public:
    TipcPublisher() = default;
    ~TipcPublisher();

    TipcPublisher(const TipcPublisher&)            = delete;
    TipcPublisher& operator=(const TipcPublisher&) = delete;

    // Bind the listening socket at the artheia-declared TIPC address.
    // Returns true on success. On failure (kernel without AF_TIPC,
    // permission denied, etc.) the publisher still functions but
    // does nothing — keeps the supervisor binary usable on hosts that
    // don't have TIPC kernel support.
    bool open(uint32_t tipc_type, uint32_t tipc_instance);
    void close();
    bool is_open() const { return listen_fd_ >= 0; }

    // Publish a binary message as one SEQPACKET datagram to every
    // connected client. Length is implicit in SOCK_SEQPACKET so callers
    // don't need to length-prefix; one publish() == one recv() on the
    // GUI side. Payload is whatever the .proto serialization produced.
    //
    // Each datagram is framed with a 2-byte type tag at offset 0 so the
    // receiver knows which protobuf message to decode:
    //   0x0001 SupervisionEvent
    //   0x0002 HealthBeacon
    //   0x0003 TreeSnapshot
    void publish(uint16_t type_tag, const std::string& serialized);

    // Drain pending connect / disconnect / inbound-msg events. Called
    // once per supervisor main-loop iteration; non-blocking, returns
    // immediately if nothing is ready.
    void poll();

private:
    int              listen_fd_ = -1;
    std::vector<int> clients_;
    int              epoll_fd_  = -1;

    void accept_clients();
    void drop_client(int fd);
};

}  // namespace supervisor

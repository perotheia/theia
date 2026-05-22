// TCP publisher for the supervisor.
//
// Binds AF_INET SOCK_STREAM on 0.0.0.0:<port> (default 7610). Fans
// out events / health / snapshots to every connected client (today:
// supervisor-gui). Replaces TIPC because we want the supervisor GUI
// to attach from off-host without needing the kernel TIPC module.
//
// Wire format (per-frame, repeated):
//
//   uint32_be  payload_len    // bytes that follow this header
//   uint16_be  type_tag       // 0x0001 SupervisionEvent
//                             // 0x0002 HealthBeacon
//                             // 0x0003 TreeSnapshot
//   bytes      protobuf_msg   // payload_len-2 bytes of nanopb-encoded proto
//
// payload_len includes the 2-byte tag so a client can skip an unknown
// frame by reading exactly payload_len bytes after the length header.
// The schemas (SupervisionEvent / HealthBeacon / TreeSnapshot) come
// from services/supervisor/generated/proto.
//
// Inbound bytes are drained and ignored — the control RPC wires in
// when the GUI exercises it.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace supervisor {

class TcpPublisher {
public:
    static constexpr uint16_t kDefaultPort = 7610;

    TcpPublisher() = default;
    ~TcpPublisher();

    TcpPublisher(const TcpPublisher&)            = delete;
    TcpPublisher& operator=(const TcpPublisher&) = delete;

    // Bind on the given port (host byte order). Returns true on
    // success. On failure (EADDRINUSE, etc.) the publisher stays
    // inert — the supervisor still runs, just without GUI connectivity.
    bool open(uint16_t port);
    void close();
    bool is_open() const { return listen_fd_ >= 0; }

    // Publish one framed message to every connected client.
    // tag/payload semantics: see file header. payload is the
    // protobuf bytes; framing is added here.
    void publish(uint16_t type_tag, const std::string& serialized);

    // Drain pending connect / disconnect / inbound-msg events. Called
    // once per supervisor main-loop iteration; non-blocking.
    void poll();

private:
    int              listen_fd_ = -1;
    std::vector<int> clients_;
    int              epoll_fd_  = -1;

    void accept_clients();
    void drop_client(int fd);
};

}  // namespace supervisor

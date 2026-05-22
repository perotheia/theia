// TCP client for supervisor-gui.
//
// Connects to one supervisor at TCP <host>:<port>, runs a background
// thread that reads framed protobuf messages, and invokes a callback
// for every decoded frame. The main frame consumes those and routes
// to the appropriate panel.
//
// Frame format (matches services/supervisor/src/tcp_publisher.cpp):
//
//   uint32_be payload_len    // bytes that follow this header
//   uint16_be type_tag       // 0x0001 SupervisionEvent
//                            // 0x0002 HealthBeacon
//                            // 0x0003 TreeSnapshot
//   bytes      protobuf      // payload_len-2 bytes
//
// One client per machine — the GUI instantiates N of these from the
// rig's GUI manifest (machines.yaml). Each frame is labelled with the
// owning machine_name so panels can present per-machine views.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace sup_gui {

// Callback signature: (machine_name, type_tag, payload_bytes). Invoked
// from the background thread — the implementation must marshal to the
// wx main thread itself (typically wxQueueEvent).
using FrameCallback =
    std::function<void(const std::string& machine_name,
                       uint16_t type_tag,
                       std::string payload)>;

class TcpClient {
public:
    TcpClient(std::string machine_name,
              std::string host,
              uint16_t port,
              FrameCallback on_frame);
    ~TcpClient();

    TcpClient(const TcpClient&)            = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    const std::string& machine_name() const { return machine_name_; }
    const std::string& host() const { return host_; }
    uint16_t           port() const { return port_; }

    // Background-thread state.
    bool is_running() const { return running_.load(); }

    // True when the socket is currently connected to the supervisor.
    // Toggles false on disconnect; the thread retries until stop().
    bool is_connected() const { return connected_.load(); }

    void start();
    void stop();

private:
    void run();
    bool connect_socket();
    void disconnect_socket();

    // Read exactly `n` bytes (or fail). Returns false on EOF/error.
    bool read_exact(void* buf, size_t n);

    std::string       machine_name_;
    std::string       host_;
    uint16_t          port_;
    FrameCallback     callback_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    int               fd_{-1};
    std::thread       thread_;
};

}  // namespace sup_gui

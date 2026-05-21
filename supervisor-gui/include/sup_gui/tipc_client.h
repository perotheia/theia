// TIPC client for supervisor-gui.
//
// Connects to the supervisor at TIPC type 0x80020001 instance 0, runs a
// background thread that reads framed protobuf datagrams, and posts a
// custom wxEvent for every decoded message. The main frame consumes
// those events and routes them to the appropriate panel.
//
// Frame format (matches services/supervisor/src/tipc_publisher.cpp):
//   bytes [0..1]  big-endian uint16 type tag:
//                   0x0001 SupervisionEvent
//                   0x0002 HealthBeacon
//                   0x0003 TreeSnapshot
//   bytes [2..]   protobuf serialization of the corresponding message.
//
// Skeleton: connection retry on failure, no control-RPC path yet (that
// lands when GetTree/RestartChild/etc. are wired in).

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace sup_gui {

// Callback signature: (type_tag, payload_bytes). Invoked from the
// background thread — the implementation must marshal to the wx main
// thread itself (typically wxQueueEvent).
using FrameCallback =
    std::function<void(uint16_t type_tag, std::string payload)>;

class TipcClient {
public:
    TipcClient(uint32_t tipc_type, uint32_t tipc_instance,
               FrameCallback on_frame);
    ~TipcClient();

    TipcClient(const TipcClient&)            = delete;
    TipcClient& operator=(const TipcClient&) = delete;

    // Returns true while the background thread is running. Doesn't say
    // anything about whether the socket is connected — that's
    // is_connected().
    bool is_running() const { return running_.load(); }

    // True when the socket is currently connected to the supervisor.
    // Toggles to false on supervisor death; the thread retries until
    // stop() is called.
    bool is_connected() const { return connected_.load(); }

    // Start the background thread. Connection is attempted asynchronously.
    void start();

    // Signal the thread to stop and join. Idempotent.
    void stop();

private:
    void run();
    bool connect_socket();
    void disconnect_socket();

    uint32_t          tipc_type_;
    uint32_t          tipc_instance_;
    FrameCallback     callback_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    int               fd_{-1};
    std::thread       thread_;
};

}  // namespace sup_gui

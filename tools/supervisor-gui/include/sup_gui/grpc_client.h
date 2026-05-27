// gRPC client for one machine's services/com bridge.
//
// One GrpcClient per row in machines.yaml. Each runs a background
// thread that opens a SupervisorView::Subscribe stream and posts
// every observation through a callback (the main frame translates
// to a wxThreadEvent so panels see the data on the wx main thread).
//
// Re-establishes the channel on disconnect — services/com may restart
// while the GUI is up.

#pragma once

#include <grpcpp/channel.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace sup_gui {

// Same shape as the old TcpClient callback so panels still take
// (machine_name, tag, payload). Tag values:
//   0x0001 SupervisionEvent
//   0x0002 HealthBeacon
//   0x0003 TreeSnapshot
using FrameCallback =
    std::function<void(const std::string& machine_name,
                       uint16_t type_tag,
                       std::string payload)>;

class GrpcClient {
public:
    GrpcClient(std::string machine_name,
                std::string host_port,
                FrameCallback on_frame);
    ~GrpcClient();

    GrpcClient(const GrpcClient&)            = delete;
    GrpcClient& operator=(const GrpcClient&) = delete;

    const std::string& machine_name() const { return machine_name_; }
    bool is_connected() const { return connected_.load(); }

    void start();
    void stop();

    // Synchronous one-shot Configure call against TraceStream (#365).
    // Returns the status from the supervisor's ControlReply (0 = OK).
    // On RPC failure returns a negative number; on parse failure -2.
    // The GUI's right-click → Configure Trace dialog (panels/
    // applications_panel.cpp) calls this from the wx main thread.
    // Blocking up to 3s is acceptable — the call is synchronous in
    // user-perception terms.
    int configure_trace(const std::string& target_node,
                         const std::string& msg_type,
                         bool enabled);

private:
    void run();

    std::string             machine_name_;
    std::string             host_port_;
    FrameCallback           callback_;
    std::atomic<bool>       running_  {false};
    std::atomic<bool>       connected_{false};
    std::thread             thread_;
    std::shared_ptr<grpc::Channel> channel_;
};

}  // namespace sup_gui

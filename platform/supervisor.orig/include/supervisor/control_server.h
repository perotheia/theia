// ControlServer — the supervisor's standard-transport control surface,
// behind a pimpl so runtime.cpp can drive it without ever including the
// nanopb ControlRequest.pb.h.
//
// The supervisor's run() loop (src/runtime.cpp) includes the LIBPROTOBUF
// ControlRequest.pb.h for its legacy on_inbound_frame path. The new
// nanopb control surface (SupervisorControlNode + TipcMux) lives in
// control_node.cpp and includes the NANOPB ControlRequest.pb.h. Both
// headers share the same basename, so they MUST NOT meet in one
// translation unit. This header exposes only opaque start/stop + a
// std::unique_ptr<Impl> — no runtime / nanopb types leak — so runtime.cpp
// can own a ControlServer's lifetime while the nanopb world stays
// confined to control_node.cpp.
//
// See docs/com-supervisor-transport.md §5 and control_node.h.

#pragma once

#include <memory>

namespace supervisor {

class Supervisor;  // fwd

class ControlServer {
public:
    explicit ControlServer(Supervisor* sup);
    ~ControlServer();

    ControlServer(const ControlServer&)            = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // Start the node + bind it on the TipcMux at the control address
    // (0x80020003/0). Returns false if the TIPC bind failed (the legacy
    // publisher control path stays available either way). Idempotent
    // enough: a failed start leaves the server inert.
    bool start();

    // Stop the mux's epoll thread and the node's worker thread. Safe to
    // call even if start() failed or was never called.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace supervisor

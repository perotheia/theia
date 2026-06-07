// sup_link — com's RemoteRef link to the supervisor's standard-transport
// control surface (#418).
//
// This replaces the control half of TipcUplink::send_control_request. The
// supervisor's control node (gen-app SupervisorCtl) is a gen_server bound on
// TipcMux at TIPC type 0x80020001, instance 0 — the current supervisor UNIFIED
// control + the event/health/snapshot firehose on this one address (the old
// split-out 0x80020003 control node is gone). com drives the control surface
// with a RemoteRef<…,0x80020001,0> + a dedicated reply-pump TipcMux, both here.
//
// The gRPC edge (ComGrpcProxy_handlers.cc) builds a NANOPB
// system_supervisor_ControlRequest from the inbound gRPC call, hands it to
// SupLink::call(), and gets a nanopb system_supervisor_ControlReply back,
// which it translates to the libprotobuf reply the gRPC client expects.
//
// nanopb-only header surface — no libprotobuf here. The supervisor control
// codecs come from //platform/supervisor:supervisor_nanopb + supervisor_codecs.hh.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace services_com {

// The control op_kind values, mirrored from the supervisor's control switch
// (platform/supervisor/src/control_node.cpp + ComGrpcProxy_handlers.cc).
enum SupOpKind : uint32_t {
    kSupOpStartChild        = 3,
    kSupOpDeleteChild       = 4,
    kSupOpRestartChild      = 5,
    kSupOpTerminateChild    = 6,
    kSupOpStop              = 7,
    kSupOpConfigureTrace    = 9,
    kSupOpGetTraceConfig    = 10,
    kSupOpConfigureLogLevel = 11,
};

// A ChildSpec carried into StartChild, expressed in primitive C++ types so
// this header stays free of both nanopb AND libprotobuf. The gRPC edge fills
// it from the libprotobuf request; SupLink re-encodes it into a nanopb
// ChildSpec inside the ControlRequest.
struct SupChildSpec {
    std::string              name;
    std::string              parent_supervisor;
    std::vector<std::string> start_cmd;
    uint32_t                 restart  = 0;
    int32_t                  shutdown = 0;
    uint32_t                 type     = 0;
    std::vector<std::string> modules;
};

// A decoded ControlReply, in primitives. trace_config_list holds the raw
// serialized services.supervisor.TraceConfigList bytes (wire-v3 identical to
// the libprotobuf encoding) for the GetTraceConfig read-back. tree_snapshot
// holds the raw system_supervisor.TreeSnapshot bytes for the GetTree poll
// (the firehose Subscribe streams these on an interval — the pull model).
struct SupReply {
    uint32_t    status = 0;
    std::string message;
    std::string child_name;
    std::string trace_config_list;   // raw proto bytes (may be empty)
    std::string tree_snapshot;       // raw TreeSnapshot bytes (GetTree)
};

// Singleton link to the supervisor control node. Opened once by
// ComGrpcProxy::do_start, torn down by do_stop. Thread-safe.
//
// All ControlRequest building + ControlReply decoding happens inside the
// implementation (sup_link.cc), which is the only TU that touches the nanopb
// supervisor structs — keeping the same-basename ControlRequest.pb.h out of
// the libprotobuf gRPC edge in ComGrpcProxy_handlers.cc.
class SupLink {
public:
    static SupLink& instance();

    // Connect the RemoteRef (TIPC type 0x80020001/0) and start the reply
    // pump. Returns false if the supervisor isn't reachable. Idempotent.
    bool start(int connect_timeout_ms = 3000);

    // Stop the reply pump + drop the connection.
    void stop();

    bool connected() const;

    // ---- op-specific forwards (build nanopb req, RemoteRef-call, decode) ---
    // Each returns false on transport error / timeout / not-connected.
    bool start_child(const SupChildSpec& spec, SupReply& out,
                     int timeout_ms = 5000);
    bool name_op(uint32_t op_kind, const std::string& name, SupReply& out,
                 int timeout_ms = 5000);   // Delete/Restart/Terminate
    bool stop_supervisor(SupReply& out, int timeout_ms = 5000);
    bool configure_log_level(const std::string& target_node,
                             const std::string& level, SupReply& out,
                             int timeout_ms = 5000);
    bool configure_trace(const std::string& target_node,
                         const std::string& msg_type, bool enabled,
                         uint32_t kind, SupReply& out, int timeout_ms = 5000);
    bool get_trace_config(SupReply& out, int timeout_ms = 5000);

    // GetTree — a point-in-time TreeSnapshot. The firehose Subscribe polls
    // this on an interval and re-emits each snapshot (the supervisor's event
    // firehose has no remote egress; GetTree is the live source — same model
    // as `tdb ps --follow`). Raw TreeSnapshot bytes land in out.tree_snapshot.
    bool get_tree(SupReply& out, int timeout_ms = 3000);

private:
    SupLink();
    ~SupLink();
    SupLink(const SupLink&)            = delete;
    SupLink& operator=(const SupLink&) = delete;

    uint32_t next_correlation_id();

    struct Impl;
    Impl* impl_;   // pimpl so this header stays free of runtime/TipcMux types
};

}  // namespace services_com

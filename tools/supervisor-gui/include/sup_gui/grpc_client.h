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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sup_gui {

// Same shape as the old TcpClient callback so panels still take
// (machine_name, tag, payload). Tag values:
//   0x0001 SupervisionEvent   (supervisor lifecycle — not emitted under the
//                              snapshot-only pull model; reserved)
//   0x0002 HealthBeacon
//   0x0003 TreeSnapshot
//   0x0004 SystemInfo         (GetSystemInfo — host + build facts)
//   0x0005 TraceRecord        (TraceStream egress — live message traces :7710)
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
                         bool enabled, uint32_t kind = 0);

    // Child lifecycle (Processes panel context menu). Both return per's
    // ControlReply status (0 = OK), negative on RPC failure; msg gets the
    // reply message. Kill restarts; Remove stop-and-holds (no policy restart).
    int restart_child(const std::string& name, std::string* msg = nullptr);
    int terminate_child(const std::string& name, std::string* msg = nullptr);

    // ---- Persistency (services/per) proxy — PerView on the SAME :7700 -----
    // One (config_type, digest) schema-registry row.
    struct SchemaRow { std::string config_type; std::string digest; };
    // ListSchemas(config_type="" → all). Returns the registry rows (empty on
    // RPC failure — check ok). Synchronous, from the wx main thread.
    std::vector<SchemaRow> list_schemas(const std::string& config_type,
                                        bool* ok = nullptr);
    // Snapshot(label) — trigger a per config backup. Returns per's status
    // (0 = OK) + fills msg with per's reply message; negative on RPC failure.
    int snapshot(const std::string& label, std::string* msg = nullptr);

    // ---- Crash forensics (Applications panel "Download tombstone") --------
    // A crashed child's tombstone bytes (capped at the supervisor under the
    // TIPC reply limit). When `truncated`, `path` is the full file's on-host
    // location so the operator can read the rest at the source.
    struct TombstoneResult {
        bool        found       = false;  // false: no tombstone (never crashed)
        bool        truncated   = false;  // content shorter than total_bytes
        uint32_t    total_bytes = 0;      // full file size at the source
        std::string path;                 // on-host path of the full file
        std::string content;              // the (capped) tombstone text
    };
    // GetTombstone(child_name). ok=false on RPC failure (left untouched on a
    // valid found=false reply). Synchronous, from the wx main thread.
    TombstoneResult get_tombstone(const std::string& child_name,
                                  bool* ok = nullptr);

private:
    void run();          // SupervisorView Subscribe (:7700) + GetSystemInfo
    void run_trace();    // TraceStream Subscribe (:7710) — live trace records

    // Derive the trace endpoint from host_port_: same host, port 7710 (com's
    // TraceForwarder). Overridable via $THEIA_COM_TRACE_LISTEN host:port.
    std::string trace_endpoint() const;

    // Shared impl for restart_child / terminate_child (ChildSelector RPC).
    int child_op(const std::string& name, bool no_restart,
                 const char* which, std::string* msg);

    std::string             machine_name_;
    std::string             host_port_;
    FrameCallback           callback_;
    std::atomic<bool>       running_  {false};
    std::atomic<bool>       connected_{false};
    std::thread             thread_;
    std::thread             trace_thread_;
    std::shared_ptr<grpc::Channel> channel_;
    std::shared_ptr<grpc::Channel> trace_channel_;

    // The live stream ClientContexts, so stop() can TryCancel() a blocked
    // reader->Read() (resetting the channel shared_ptr does NOT cancel an
    // in-flight RPC — the reader holds its own ref, so Read() would hang
    // until the next record, which never comes when no trace is enabled).
    // Stored as void* to keep the grpc ClientContext type (a version-dependent
    // typedef, awkward to forward-declare) out of this header; the .cpp casts.
    // Guarded: the worker threads set/clear them, stop() reads them.
    std::mutex              ctx_mu_;
    void*                   sub_ctx_   {nullptr};
    void*                   trace_ctx_ {nullptr};
};

}  // namespace sup_gui

// trace_link — com's PG member for log[trace]'s TraceRecord group.
//
// The trace egress lives IN com (TraceForwarder runnable). TraceLink is the
// in-Theia half: it pg_joins the TraceRecord group — the supervisor allocates
// com's delivery address — and TraceStreamPump PG-multicasts every record to it
// (the SAME live stream tdb tracecat / artheia.observer get). Each record's raw
// proto-wire bytes are handed to a sink callback; TraceForwarder_handlers.cc
// parses them into a libprotobuf services.com.TraceRecord and fans out to gRPC
// subscribers. (Pre-PG this was a SEQPACKET Subscribe to TraceCtl — both gone.)
//
// The PgClient world is confined to trace_link.cc so this header stays free of
// runtime/TIPC types. The sink takes RAW bytes (no proto type crosses this
// header), so the gRPC side re-parses them into its own libprotobuf
// TraceRecord — byte-identical.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace services_com {

// A subscriber sink: called for each record with its raw proto-wire bytes
// (a serialized system_services_log_TraceRecord == services.com.TraceRecord).
using TraceSink = std::function<void(const std::string& record_bytes)>;

// Singleton link to the trace hub. Opened once by TraceForwarder::do_start,
// torn down by do_stop. Thread-safe.
class TraceLink {
public:
    static TraceLink& instance();

    // Install the fan-out sink BEFORE start() so no record is missed once the
    // backlog spill begins. Replaces any previous sink.
    void set_sink(TraceSink sink);

    // Bind the subscriber socket, gen_call SubscribeReq to TraceCtl, and start
    // the accept/recv thread. Returns false if the hub isn't reachable (the
    // collector not up yet). Idempotent.
    bool start(int connect_timeout_ms = 3000);

    // Stop the recv thread + close sockets.
    void stop();

    bool running() const;

private:
    TraceLink();
    ~TraceLink();
    TraceLink(const TraceLink&)            = delete;
    TraceLink& operator=(const TraceLink&) = delete;

    struct Impl;
    Impl* impl_;   // pimpl so this header stays free of runtime/TIPC types
};

}  // namespace services_com

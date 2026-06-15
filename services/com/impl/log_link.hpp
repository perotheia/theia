// log_link — com's subscriber to log[logging]'s TIPC log hub.
//
// The LOG analogue of trace_link (impl/trace_link.hpp). The log egress lives in
// com (LogForwarder runnable). LogLink is the TIPC half: it binds a SEQPACKET
// subscriber socket, gen_calls LogSubscribeReq to log[logging]'s LogDaemon
// (0x80010003/0) with that address, then accepts the hub's connection and
// receives raw LogRecord casts (the SAME path tdb logcat /
// artheia.observer.LogObserver uses). Each record's raw proto-wire bytes are
// handed to a sink callback — LogForwarder_handlers.cc parses them into a
// libprotobuf services.com.LogRecord and fans out to gRPC subscribers.
//
// Like trace_link, the nanopb world (LogSubscribeReq encode, the TipcMux/
// RemoteRef) is confined to log_link.cc so it never meets the libprotobuf gRPC
// edge. The sink takes RAW bytes (no proto type crosses this header), so the
// gRPC side re-parses them into its own libprotobuf LogRecord — byte-identical.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace services_com {

// A subscriber sink: called for each record with its raw proto-wire bytes
// (a serialized system_services_log_LogRecord == services.com.LogRecord).
using LogSink = std::function<void(const std::string& record_bytes)>;

// Singleton link to the log hub. Opened once by LogForwarder::do_start, torn
// down by do_stop. Thread-safe.
class LogLink {
public:
    static LogLink& instance();

    // Install the fan-out sink BEFORE start() so no record is missed once the
    // backlog spill begins. Replaces any previous sink.
    void set_sink(LogSink sink);

    // Bind the subscriber socket, gen_call LogSubscribeReq to LogDaemon, and
    // start the accept/recv thread. Returns false if the hub isn't reachable.
    // Idempotent.
    bool start(int connect_timeout_ms = 3000);

    // Stop the recv thread + close sockets.
    void stop();

    bool running() const;

private:
    LogLink();
    ~LogLink();
    LogLink(const LogLink&)            = delete;
    LogLink& operator=(const LogLink&) = delete;

    struct Impl;
    Impl* impl_;   // pimpl so this header stays free of runtime/TIPC types
};

}  // namespace services_com

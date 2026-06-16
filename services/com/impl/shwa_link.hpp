// shwa_link — com's receiver for SHWA's AccelTelemetry egress.
//
// The hardware-telemetry analogue of log_link / trace_link. SHWA broadcasts each
// AccelSample as a GEN_CAST over TIPC SOCK_DGRAM to a well-known AccelTelemetry
// egress service name (services/shwa AccelSubmitter — mirror of the runtime's
// TraceSubmitter). AccelTelemetry is a plain senderReceiver broadcast, so there
// is NO Subscribe RPC: SHWA sends regardless; com just BINDS the egress name and
// recvs. ShwaLink hands each sample's RAW proto-wire bytes to a sink callback —
// ComGrpcProxy parses them into a libprotobuf services.com.AccelSample and folds
// them into the SupervisorView Subscribe firehose, GATED on a connected gRPC
// subscriber (so com forwards only while the GUI is attached).
//
// Like log_link, the nanopb/TIPC world stays here; the sink takes RAW bytes (no
// proto type crosses this header) so the gRPC side re-parses them into its own
// libprotobuf AccelSample — byte-identical wire.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace services_com {

// A subscriber sink: called for each sample with its raw proto-wire bytes
// (a serialized system_services_shwa_AccelSample == services.com.AccelSample).
using AccelSink = std::function<void(const std::string& sample_bytes)>;

// Singleton link to SHWA's AccelTelemetry egress. Opened once by
// ComGrpcProxy::do_start, torn down by do_stop. Thread-safe.
class ShwaLink {
public:
    static ShwaLink& instance();

    // Install the sink BEFORE start() so no sample is missed. Replaces any
    // previous sink.
    void set_sink(AccelSink sink);

    // Bind the AccelTelemetry egress DGRAM name and start the recv thread.
    // Returns false only if the socket/bind fails (a quiet SHWA is fine — the
    // socket just receives nothing). Idempotent.
    bool start();

    // Stop the recv thread + close the socket.
    void stop();

    bool running() const;

private:
    ShwaLink();
    ~ShwaLink();
    ShwaLink(const ShwaLink&)            = delete;
    ShwaLink& operator=(const ShwaLink&) = delete;

    struct Impl;
    Impl* impl_;   // pimpl so this header stays free of runtime/TIPC types
};

}  // namespace services_com

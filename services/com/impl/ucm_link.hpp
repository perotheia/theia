// ucm_link — com's RemoteRef link to services/ucm's UcmDaemon (the ara::ucm
// agent ctl front), so com can proxy UCM's RequestUpdate + progress over gRPC
// (UcmView) for an OTA client (GS / VUCM). Mirrors per_link/sup_link: the nanopb
// ucm structs + the RemoteRef/TipcMux live ONLY in ucm_link.cc, so the libprotobuf
// gRPC edge (ComGrpcProxy_handlers.cc) never meets the nanopb ucm headers.
//
// UcmDaemon is at TIPC 0x8001000E/0 (UpdateCtl.RequestUpdate). The progress
// sample comes from UcmFsm's UcmProgressStream (0x8001001E); v1 reads the LAST
// progress the link has seen (subscribed in start()), enough for the GS poll.

#pragma once

#include <cstdint>
#include <string>

namespace services_com {

// A package-update request in primitives (the gRPC edge → nanopb PackageManifest).
struct UcmUpdateReq {
    std::string name;            // "theia" (full) or an FC name (partial)
    std::string version;
    uint32_t    kind  = 0;       // UpdateKind: 0=SOFTWARE 1=CONFIG
    uint32_t    scope = 0;       // UpdateScope: 0=FULL 1=PARTIAL
    std::string artifact_path;   // staged tar/release ("" = current)
    std::string signature;
};

// One decoded UcmProgress sample, in primitives.
struct UcmProgressSample {
    uint32_t    state = 0;       // UcmState ordinal (0 IDLE..7 ACTIVE, 8 ROLLBACK)
    std::string version;
    uint32_t    kind  = 0;
    uint32_t    scope = 0;
    std::string detail;
    uint64_t    ts_ns = 0;
    bool        valid = false;   // false = no sample yet
};

// Singleton link to UcmDaemon (+ the UcmFsm progress stream). Opened by
// ComGrpcProxy::do_start, torn down by do_stop. Thread-safe.
class UcmLink {
public:
    static UcmLink& instance();

    // Connect the RemoteRef (TIPC 0x8001000E/0) + subscribe to the progress
    // stream + start the reply pump. Returns false if ucm isn't reachable.
    bool start(int connect_timeout_ms = 3000);
    void stop();
    bool connected() const;

    // RequestUpdate(manifest) → UcmReply.status (0=accepted 1=reject 2=not-ready).
    // Returns false on transport error / timeout / not-connected.
    bool request_update(const UcmUpdateReq& req, uint32_t& status_out,
                        int timeout_ms = 6000);

    // The latest UcmProgress sample the link has observed (the FSM state). When
    // no sample has arrived yet, returns a sample with valid=false.
    UcmProgressSample latest_progress();

private:
    UcmLink();
    ~UcmLink();
    UcmLink(const UcmLink&)            = delete;
    UcmLink& operator=(const UcmLink&) = delete;

    struct Impl;
    Impl* impl_;   // pimpl so this header stays free of runtime/TIPC types
};

}  // namespace services_com

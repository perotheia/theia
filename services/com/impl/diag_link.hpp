// diag_link — com's RemoteRef link to services/diag's UdsRouter, so com can
// proxy a UDS request (read a DID, read DTCs) over gRPC (DiagView) for the GUI /
// a tester — no DoIP TCP client or TIPC client needed. Mirrors nm_link/per_link:
// the nanopb diag structs + the RemoteRef/TipcMux live ONLY in diag_link.cc, so
// the libprotobuf gRPC edge (ComGrpcProxy_handlers.cc) never meets the nanopb
// diag headers.
//
// UdsRouter is at TIPC 0x80010018/0, op Handle(UdsRequest) → UdsReply.

#pragma once

#include <cstdint>
#include <string>

namespace services_com {

// A UDS exchange in primitives (mirrors system_services_diag_UdsRequest/Reply).
struct DiagUdsResult {
    std::string uds;          // the response UDS bytes
    bool        is_nrc = false;  // a Negative Response (7F sid nrc)
    bool        ok = false;   // false = link unreachable / no reply
};

// Singleton link to UdsRouter. Opened by ComGrpcProxy::do_start, torn down by
// do_stop. Thread-safe.
class DiagLink {
public:
    static DiagLink& instance();

    // Connect the RemoteRef (TIPC 0x80010018/0) + start the reply pump.
    // Returns false if diag isn't reachable. Idempotent.
    bool start(int connect_timeout_ms = 3000);
    void stop();
    bool connected() const;

    // SendUds: run one UDS request (target_addr + raw bytes) through the router.
    DiagUdsResult send_uds(uint32_t target_addr, const std::string& uds,
                           int timeout_ms = 8000);

private:
    DiagLink();
    ~DiagLink();
    DiagLink(const DiagLink&) = delete;
    DiagLink& operator=(const DiagLink&) = delete;
    struct Impl;
    Impl* impl_;
};

}  // namespace services_com

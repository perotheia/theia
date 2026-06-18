// State struct for UdsRouter — APP-OWNED, WRITE-ONCE.
//
// `artheia gen-app --kind fc` seeds this once and then NEVER overwrites
// it (unless --force). The node's persistent state is app behaviour,
// which gen-app does NOT derive from the .art — so you own this file.
// Add the fields the node's handlers / init() need here; the generated
// lib/UdsRouter.hh #includes it and binds it as the GenServer
// State type.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ara::diag {

struct UdsRouterState {
    // ---- UDS session (0x10 DiagnosticSessionControl) -----------------------
    // 0x01 default, 0x02 programming, 0x03 extended. Reverts on the S3 timer.
    uint8_t  session = 0x01;

    // ---- applied DiagConfig ------------------------------------------------
    uint32_t session_timeout_ms = 5000;

    // ---- running fault LOG (fed by phm's PhmDtcStream) ---------------------
    // Each PhmFaultEvent phm casts is appended here; 0x22 fault-log DIDs read by
    // index (0xFD00 + idx → the Nth record), and 0x19 reports them as DTCs. The
    // "fault idx" the diag config exposes over UDS. Capped (ring) so a long run
    // doesn't grow unbounded.
    struct FaultRec {
        std::string entity;   // the FC/worker the fault is about
        uint32_t    level = 0;   // HealthLevel (0 OK..3 FAILED)
        uint32_t    kind  = 0;   // SupervisionKind
        uint64_t    ts_ns = 0;
    };
    std::vector<FaultRec> fault_log;
    static constexpr size_t kMaxFaults = 64;
};

}  // namespace ara::diag

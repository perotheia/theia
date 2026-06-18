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

namespace ara::diag {

struct UdsRouterState {
    // ---- UDS session (0x10 DiagnosticSessionControl) -----------------------
    // 0x01 default, 0x02 programming, 0x03 extended. Non-default sessions gate
    // the security-protected services + revert on the S3 timer.
    uint8_t  session = 0x01;

    // ---- UDS security (0x27 SecurityAccess) --------------------------------
    bool     security_unlocked = false;   // a passed 0x27 unlocks write/program
    bool     seed_pending      = false;   // requestSeed sent, awaiting sendKey
    uint32_t security_attempts = 0;       // bad-key counter (NRC 0x36 at the cap)
    std::string last_seed;                // the seed bytes the key is checked against

    // ---- applied DiagConfig ------------------------------------------------
    uint32_t session_timeout_ms = 5000;
    std::string security_key_slot = "diag_tester_cert";
};

}  // namespace ara::diag

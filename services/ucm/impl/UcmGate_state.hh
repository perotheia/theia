// State struct for UcmGate — APP-OWNED, WRITE-ONCE.
//
// `artheia gen-app --kind fc` seeds this once and then NEVER overwrites
// it (unless --force). The node's persistent state is app behaviour,
// which gen-app does NOT derive from the .art — so you own this file.
// Add the fields the node's handlers / init() need here; the generated
// lib/UcmGate.hh #includes it and binds it as the GenServer
// State type.

#pragma once

#include <cstdint>
#include <string>

namespace ara::ucm {

struct UcmGateState {
    // Applied UcmConfig (etcd-backed; on_config_update refreshes it).
    std::string releases_root    = "/opt/theia/releases";
    uint32_t    verify_budget_ms = 30000;
    uint32_t    retain_releases  = 3;

    // The in-flight update being driven through the FSM. Copied from the pending
    // manifest on EvStartUpdate so a later RequestUpdate can't mutate it
    // mid-flight. `verifying` guards the one-shot VERIFYING timer.
    std::string version;        // target version of the update in flight
    uint32_t    kind  = 0;      // UpdateKind (0=SOFTWARE, 1=CONFIG)
    uint32_t    scope = 0;      // UpdateScope (0=FULL, 1=PARTIAL)
    bool        verifying = false;

    // Two-phase commit. confirm_window_ms > 0 (from the manifest `requires`
    // "confirm=<ms>") means: after the clean verify window, HOLD in PROVISIONAL
    // and wait for a remote Confirm; if none by the deadline, auto-rollback. The
    // marker (version/campaign_id/deadline) is persisted to per/etcd so a reboot
    // mid-window resumes PROVISIONAL. `provisioning` guards the one-shot deadline
    // timer (analogous to `verifying`).
    uint32_t    confirm_window_ms = 0;     // 0 = no confirm required (legacy commit)
    std::string campaign_id;
    uint64_t    confirm_deadline_ns = 0;
    bool        provisioning = false;      // in PROVISIONAL, deadline timer armed
};

}  // namespace ara::ucm

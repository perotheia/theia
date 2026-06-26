// State struct for VucmGate — APP-OWNED, WRITE-ONCE.
//
// `artheia gen-app --kind fc` seeds this once and then NEVER overwrites
// it (unless --force). The node's persistent state is app behaviour,
// which gen-app does NOT derive from the .art — so you own this file.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "system/services/vucm/vucm.pb.h"   // CampaignState

namespace ara::vucm {

// The L4-B campaign roster + aggregate progress. VucmGate is the vehicle
// coordinator — it drives the VucmCampaign FSM and aggregates every board's
// PROVISIONAL marker from the SHARED etcd keyspace (per-board key
// ucm_activation_<board>), then fans the aggregate Confirm/Cancel.
struct VucmGateState {
    // ── Static config (read once in init() from the per-FC params) ──────────
    // The boards every campaign fans out to (VucmConfig.boards, comma-list).
    std::vector<std::string> cfg_boards;
    // The CMP_CONFIRMING aggregate-wait budget (ms) before giving up → Cancel.
    uint32_t                 confirm_budget_ms = 120000;
    // This board's own machine name (THEIA_MACHINE) — the single-board fallback.
    std::string              self_board = "central";

    // ── Active campaign (empty campaign_id == idle) ─────────────────────────
    std::string campaign_id;
    std::string version;
    uint32_t    scope = 0;
    // The boards THIS campaign targets (snapshot of cfg_boards at accept time).
    std::vector<std::string> boards;
    // Confirm-poll ticks elapsed (the barrier budget counter).
    uint32_t    confirm_ticks = 0;
    // Last broadcast campaign state (served by GetCampaignStatus).
    system_services_vucm_CampaignState last_state =
        system_services_vucm_CampaignState_CampaignState_CMP_IDLE;
};

}  // namespace ara::vucm

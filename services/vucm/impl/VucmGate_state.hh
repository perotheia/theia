// State struct for VucmGate — APP-OWNED, WRITE-ONCE.
//
// `artheia gen-fc` seeds this once and then NEVER overwrites
// it (unless --force). The node's persistent state is app behaviour,
// which gen-fc does NOT derive from the .art — so you own this file.

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
    // L4-C/D: the bundle base (S3 dir of per-board role .mender artifacts); passed
    // as the RequestUpdate manifest artifact_path so each UCM resolves its role.
    std::string              bundle_base;
    // This board's own machine name (THEIA_MACHINE) — the single-board fallback.
    std::string              self_board = "central";
    // The deploy prefix's releases dir (<root>/releases) — where V-UCM reads the
    // target release's migration/{migration.json,plugin.so} to drive the CENTRAL
    // config migration (Snapshot + MigrateBulk) before the fan-out. Default matches
    // UCM's; overridable via VucmConfig.releases_root.
    std::string              releases_root = "/opt/theia/releases";

    // ── Update admission (the AUTHORIZING conjunction; VucmConfig knobs) ─────
    // enforce_* false = observe-only (lab default). last_* track the live
    // SM/NM/PHM edges; UINT32_MAX = never seen (blocks when enforced).
    bool     enforce_sm  = false;
    bool     enforce_nm  = false;
    bool     enforce_phm = false;
    uint32_t min_net_state    = 6;          // NETWORK_OPERATIONAL
    uint32_t window_start_min = 0;          // garage window, minutes-of-day UTC
    uint32_t window_end_min   = 0;          // 0/0 = no window
    bool     require_user_confirm = false;  // force the PROVISIONAL confirm leg
    bool     auto_confirm_in_window = false; // garage case 2: pre-consent → auto-Confirm in-window
    uint32_t last_sm  = 0xFFFFFFFFu;        // SmState
    uint32_t last_nm  = 0xFFFFFFFFu;        // NetState
    uint32_t last_phm = 0u;                 // HealthLevel (0 OK — absent = OK)
    // An admission-blocked campaign re-checks on a timer (authorize poll).
    bool     authorize_pending = false;

    // ── Active campaign (empty campaign_id == idle) ─────────────────────────
    std::string campaign_id;
    std::string version;
    uint32_t    scope = 0;
    // The boards THIS campaign targets (snapshot of cfg_boards at accept time).
    std::vector<std::string> boards;
    // Confirm-poll ticks elapsed (the barrier budget counter).
    uint32_t    confirm_ticks = 0;
    // Set once V-UCM has run the central config migration for THIS campaign, so
    // the Cancel/rollback path knows to RestoreSnapshot("pre-<ver>") centrally.
    bool        config_migrated = false;
    // Last broadcast campaign state (served by GetCampaignStatus).
    system_services_vucm_CampaignState last_state =
        system_services_vucm_CampaignState_CampaignState_CMP_IDLE;
};

}  // namespace ara::vucm

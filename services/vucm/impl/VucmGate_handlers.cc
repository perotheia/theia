// User handler bodies for VucmGate — the L4-B vehicle campaign orchestrator.
//
// VucmGate is the ONLY TIPC-reachable VUCM surface. It serves VucmCtlIf
// (Mender/com → CheckForCampaign), drives the VucmCampaign FSM through the
// campaign lifecycle, and runs the L4-B aggregate barrier: fan RequestUpdate to
// the boards, wait for EVERY board's PROVISIONAL marker in the SHARED etcd
// keyspace (ucm_activation_<board>), then fan the aggregate Confirm so all boards
// activate together — or on any-fail/timeout, fan Cancel so all roll back.
//
// The gate↔FSM idiom mirrors ucm (UcmGate↔UcmFsm): the gate post_event()s the
// .art transition triggers into the campaign statem (for the STATE + the
// CampaignProgress broadcast) AND re-posts the next lifecycle step to ITSELF to
// drive the chain a mailbox step at a time (no deep recursion).

#include "lib/VucmGate.hh"
#include "lib/VucmCampaign.hh"      // the campaign FSM the gate post_event()s into
#include "impl/VucmGate_state.hh"

#include "GenStateM.hh"            // theia::runtime::post_event
#include "NodeRef.hh"             // LocalRef / RemoteRef / cast / call
#include "TimerService.hh"        // send_after / process_timers (confirm poll)
#include "TipcMux.hh"             // reply pump for the per / ucm calls
#include "RemoteCodec.hh"         // THEIA_DECLARE_REMOTE_CODEC
#include "ParamsConfig.hh"        // get_config() — boards / confirm budget
#include "system/services/per/per.pb.h"
#include "system/services/ucm/ucm.pb.h"

#include <pb_decode.h>
#include <pb_encode.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

// NOTE: the per (GetConfigReq/ConfigSnapshot) + ucm (ConfirmRequest/CancelRequest/
// UcmReply) RemoteCodecs are already declared by the generated vucm_codecs.hh
// (pulled via lib/VucmGate.hh) — the to_per + to_ucm client ports put them in the
// codec set. Re-declaring them here would redefine RemoteCodec<T>. UcmActivation
// (the marker payload) is decoded inline below, not over a port, so it needs no
// codec — only pb_decode of the raw bytes from the ConfigSnapshot.

namespace ara::vucm {

// ---- IMPL-owned shared singletons (the gate→campaign wiring) ---------------
// The campaign FSM peer. VucmCampaign::on_enter publishes itself here on first
// entry; the gate post_event's the transition triggers into it.
theia::runtime::LocalRef<VucmCampaign>& vucm_campaign_ref() {
    static theia::runtime::LocalRef<VucmCampaign> ref;
    return ref;
}
// The gate peer. VucmGate::init publishes itself here so handlers can re-post the
// NEXT lifecycle event onto the gate's own mailbox (drive the chain).
theia::runtime::LocalRef<VucmGate>& vucm_gate_ref() {
    static theia::runtime::LocalRef<VucmGate> ref;
    return ref;
}

namespace {

// Advance the campaign FSM (→ CampaignProgress broadcast for that state).
template <typename Evt>
void to_fsm(const char* name, Evt evt) {
    auto& ref = vucm_campaign_ref();
    if (!ref.valid()) {
        std::fprintf(stderr, "[vucm_gate] %s before FSM wired — dropping\n", name);
        return;
    }
    theia::runtime::post_event(ref.target(), std::move(evt));
}
// Re-post the next gate step onto the gate's OWN mailbox (a mailbox step, not
// recursion — runs after the current handler returns).
template <typename Evt>
void to_gate(Evt evt) {
    auto& ref = vucm_gate_ref();
    if (ref.valid()) cast(ref, std::move(evt));
}

// ---- per (shared-etcd) link — the SOLE etcd reader. Lazily connected, its own
//      TipcMux reply pump. Mirrors UcmGate's PerLink exactly.
struct PerLink {
    struct PerClientTag { static constexpr const char* kNodeName = "per_client"; };
    using PerRef = ::theia::runtime::RemoteRef<PerClientTag, 0x80010007u, 0u>;
    PerRef                    ref;
    ::theia::runtime::TipcMux mux;
    bool                      started = false;
    std::mutex                mu;
    static PerLink& instance() { static PerLink l; return l; }
    bool ensure_started() {
        if (started) return true;
        if (!ref.connect(/*timeout_ms=*/2000)) return false;
        mux.watch_remote_ref(ref);
        mux.start();
        started = true;
        return true;
    }
};

// ---- ucm (local UpdateCtl) link — drives Confirm / Cancel on THIS board's UCM.
//      Remote boards' UCMs are driven by their OWN Mender agent (the artifact
//      transport); the aggregate read is the shared-etcd marker. For the lab
//      2-board test the central drives its local UCM directly here.
struct UcmLink {
    struct UcmDaemonTag { static constexpr const char* kNodeName = "ucm_daemon"; };
    using UcmRef = ::theia::runtime::RemoteRef<UcmDaemonTag, 0x8001001Eu, 0u>;
    UcmRef                    ref;
    ::theia::runtime::TipcMux mux;
    bool                      started = false;
    std::mutex                mu;
    static UcmLink& instance() { static UcmLink l; return l; }
    bool ensure_started() {
        if (started) return true;
        if (!ref.connect(/*timeout_ms=*/2000)) return false;
        mux.watch_remote_ref(ref);
        mux.start();
        started = true;
        return true;
    }
};

// Read one board's PROVISIONAL marker from the SHARED etcd keyspace. Returns the
// ActivationState (0 NONE / 1 PROVISIONAL / 2 CONFIRMED / -1 error) for
// ucm_activation_<board>. The whole point of L4-B's shared etcd: V-UCM reads
// EVERY board's marker locally (its per → the central's etcd), no cross-board TIPC.
int read_board_marker(const std::string& board) {
    system_services_per_GetConfigReq req = system_services_per_GetConfigReq_init_zero;
    std::string key = "ucm_activation_" + board;
    std::snprintf(req.target_node, sizeof(req.target_node), "%s", key.c_str());

    auto& link = PerLink::instance();
    std::lock_guard<std::mutex> lk(link.mu);
    if (!link.ensure_started()) return -1;
    auto result = ::theia::runtime::call<system_services_per_ConfigSnapshot>(
        link.ref, req, 0, 3000);
    if (result.tag != ::theia::runtime::CallTag::Reply) return -1;
    if (result.reply.config.size == 0) return 0;   // no marker == NONE

    system_services_ucm_UcmActivation a = system_services_ucm_UcmActivation_init_zero;
    pb_istream_t is = pb_istream_from_buffer(result.reply.config.bytes,
                                             result.reply.config.size);
    if (!pb_decode(&is, system_services_ucm_UcmActivation_fields, &a)) return -1;
    return static_cast<int>(a.state);
}

void ucm_confirm(const std::string& campaign_id) {
    system_services_ucm_ConfirmRequest req = system_services_ucm_ConfirmRequest_init_zero;
    std::snprintf(req.campaign_id, sizeof(req.campaign_id), "%s", campaign_id.c_str());
    auto& link = UcmLink::instance();
    std::lock_guard<std::mutex> lk(link.mu);
    if (!link.ensure_started()) return;
    ::theia::runtime::call<system_services_ucm_UcmReply>(link.ref, req, 0, 3000);
}
void ucm_cancel(const std::string& campaign_id) {
    system_services_ucm_CancelRequest req = system_services_ucm_CancelRequest_init_zero;
    std::snprintf(req.campaign_id, sizeof(req.campaign_id), "%s", campaign_id.c_str());
    auto& link = UcmLink::instance();
    std::lock_guard<std::mutex> lk(link.mu);
    if (!link.ensure_started()) return;
    ::theia::runtime::call<system_services_ucm_UcmReply>(link.ref, req, 0, 3000);
}

std::vector<std::string> split_boards(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string b;
    while (std::getline(ss, b, ',')) {
        size_t a = b.find_first_not_of(" \t");
        size_t z = b.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(b.substr(a, z - a + 1));
    }
    return out;
}

constexpr unsigned kPollMs = 2000;   // confirm-poll cadence

}  // namespace

// ---- OTP init/1 — publish the gate peer + read the campaign config. ---------
void VucmGate::init(VucmGateState& s) {
    vucm_gate_ref() = theia::runtime::LocalRef<VucmGate>(*this);
    auto cfg = ::theia::runtime::get_config().node(kNodeName);
    s.cfg_boards        = split_boards(cfg.str("boards", ""));
    s.confirm_budget_ms = cfg.u32("confirm_budget_ms", 120000);
    const char* m = std::getenv("THEIA_MACHINE");
    s.self_board        = (m && *m) ? m : "central";
    s.last_state = system_services_vucm_CampaignState_CampaignState_CMP_IDLE;
    s.campaign_id.clear();
    this->log().info(std::string("vucm gate up — board=") + s.self_board +
                     " roster=" + std::to_string(s.cfg_boards.size()) +
                     " (L4-B campaign orchestrator)");
}

// ---- string handle_info — the confirm-poll timer. Polls every board's
//      PROVISIONAL marker; ALL PROVISIONAL → EvProvisioned + fan Confirm;
//      budget exceeded → EvFailed + fan Cancel.
void VucmGate::handle_info(const char* info, VucmGateState& s) {
    if (!info || std::strcmp(info, "confirm_poll") != 0) return;
    if (s.campaign_id.empty() || s.boards.empty()) return;

    size_t provisional = 0;
    for (const auto& b : s.boards) {
        int st = read_board_marker(b);   // 0 NONE 1 PROVISIONAL 2 CONFIRMED -1 err
        if (st == 1 || st == 2) ++provisional;
    }

    if (provisional == s.boards.size()) {
        this->log().info(std::string("campaign ") + s.campaign_id + ": ALL " +
                         std::to_string(s.boards.size()) +
                         " boards PROVISIONAL — fanning aggregate Confirm");
        for (size_t i = 0; i < s.boards.size(); ++i) ucm_confirm(s.campaign_id);
        s.confirm_ticks = 0;
        to_fsm("EvProvisioned", EvProvisioned{});   // CMP_CONFIRMING → VALIDATING
        to_gate(EvProvisioned{});
        return;
    }

    if (++s.confirm_ticks * kPollMs >= s.confirm_budget_ms) {
        this->log().warn(std::string("campaign ") + s.campaign_id +
                         ": CONFIRMING budget exceeded (" +
                         std::to_string(provisional) + "/" +
                         std::to_string(s.boards.size()) +
                         " PROVISIONAL) — fanning Cancel");
        for (size_t i = 0; i < s.boards.size(); ++i) ucm_cancel(s.campaign_id);
        to_fsm("EvFailed", EvFailed{});
        to_gate(EvFailed{});
        return;
    }
    ::theia::runtime::send_after(::theia::runtime::process_timers(),
                                 kPollMs, *this, "confirm_poll");
}

// ---- The fleet/Mender control surface (com → CheckForCampaign). -------------
CampaignReply VucmGate::handle_call(const CampaignRequest& req, VucmGateState& s) {
    CampaignReply reply = system_services_vucm_CampaignReply_init_zero;
    if (!s.campaign_id.empty()) {
        this->log().warn("CheckForCampaign while one is in flight — rejecting");
        reply.accepted = 0;
        reply.state = s.last_state;
        return reply;
    }
    s.campaign_id   = req.campaign_id;
    s.version       = req.version;
    s.scope         = static_cast<uint32_t>(req.scope);
    s.boards        = s.cfg_boards;
    s.confirm_ticks = 0;
    if (s.boards.empty()) s.boards.push_back(s.self_board);   // single-board fallback

    this->log().info(std::string("CheckForCampaign id=") + s.campaign_id +
                     " version=" + s.version + " boards=" +
                     std::to_string(s.boards.size()));
    to_fsm("EvDeployment", EvDeployment{});   // → CMP_PLANNING
    to_gate(EvDeployment{});
    reply.accepted = 1;
    reply.state = system_services_vucm_CampaignState_CampaignState_CMP_PLANNING;
    return reply;
}

CampaignProgress VucmGate::handle_call(const CampaignStatusReq& /*req*/, VucmGateState& s) {
    CampaignProgress p = system_services_vucm_CampaignProgress_init_zero;
    std::snprintf(p.campaign_id, sizeof(p.campaign_id), "%s", s.campaign_id.c_str());
    std::snprintf(p.version, sizeof(p.version), "%s", s.version.c_str());
    p.state = s.last_state;
    return p;
}

// ---- Lifecycle event handlers — the gate↔FSM chain. Each advances the FSM
//      (state + broadcast) and re-posts the NEXT step. The lab auto-advances the
//      PLANNING/AUTHORIZING legs (the SM/PHM go/no-go gates are observed via
//      from_sm/from_phm; a real deny would post EvBlocked / EvFailed instead).

void VucmGate::handle_cast(const EvDeployment& /*msg*/, VucmGateState& s) {
    s.last_state = system_services_vucm_CampaignState_CampaignState_CMP_PLANNING;
    to_fsm("EvPlanned", EvPlanned{});         // PLANNING → AUTHORIZING
    to_gate(EvPlanned{});
}

void VucmGate::handle_cast(const EvPlanned& /*msg*/, VucmGateState& s) {
    s.last_state = system_services_vucm_CampaignState_CampaignState_CMP_AUTHORIZING;
    to_fsm("EvAuthorized", EvAuthorized{});   // AUTHORIZING → INSTALLING
    to_gate(EvAuthorized{});
}

void VucmGate::handle_cast(const EvAuthorized& /*msg*/, VucmGateState& s) {
    // INSTALLING: each board's UCM installs (RequestUpdate / each board's Mender
    // agent), then HOLDS in PROVISIONAL. Arm the confirm-poll; the FSM moves to
    // CONFIRMING (the barrier) and the poll (handle_info) drives the rest.
    s.last_state = system_services_vucm_CampaignState_CampaignState_CMP_INSTALLING;
    this->log().info(std::string("campaign ") + s.campaign_id +
                     ": INSTALLING — boards driving to PROVISIONAL; arming confirm poll");
    s.confirm_ticks = 0;
    to_fsm("EvInstalled", EvInstalled{});     // INSTALLING → CMP_CONFIRMING
    s.last_state = system_services_vucm_CampaignState_CampaignState_CMP_CONFIRMING;
    ::theia::runtime::send_after(::theia::runtime::process_timers(),
                                 kPollMs, *this, "confirm_poll");
}

void VucmGate::handle_cast(const EvInstalled& /*msg*/, VucmGateState& /*s*/) {
    // FSM now in CMP_CONFIRMING; the confirm-poll (handle_info) drives the rest.
}

void VucmGate::handle_cast(const EvProvisioned& /*msg*/, VucmGateState& s) {
    s.last_state = system_services_vucm_CampaignState_CampaignState_CMP_VALIDATING;
    to_fsm("EvValidated", EvValidated{});     // VALIDATING → DONE
    to_gate(EvValidated{});
}

void VucmGate::handle_cast(const EvValidated& /*msg*/, VucmGateState& s) {
    s.last_state = system_services_vucm_CampaignState_CampaignState_CMP_DONE;
    this->log().info(std::string("campaign ") + s.campaign_id +
                     ": DONE — all boards confirmed ACTIVE");
    s.campaign_id.clear();
    s.boards.clear();
}

void VucmGate::handle_cast(const EvBlocked& /*msg*/, VucmGateState& /*s*/) {
    to_fsm("EvBlocked", EvBlocked{});         // AUTHORIZING → PLANNING (retry)
}

void VucmGate::handle_cast(const EvFailed& /*msg*/, VucmGateState& s) {
    s.last_state = system_services_vucm_CampaignState_CampaignState_CMP_ROLLBACK;
    this->log().warn(std::string("campaign ") + s.campaign_id + ": FAILED — rolled back");
    to_fsm("EvFailed", EvFailed{});
    s.campaign_id.clear();
    s.boards.clear();
}

// ---- Observed foreign edges (SM authorize / PHM validate). Lab: log only.
void VucmGate::handle_cast(const SmStateMsg& /*msg*/, VucmGateState& /*s*/) {
    std::fprintf(stderr, "[%s] SM state observed\n", kNodeName);
}
void VucmGate::handle_cast(const PhmHealthStatus& /*msg*/, VucmGateState& /*s*/) {
    std::fprintf(stderr, "[%s] PHM health observed\n", kNodeName);
}

}  // namespace ara::vucm

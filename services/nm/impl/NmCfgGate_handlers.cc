// User handler bodies for NmCfgGate — the config-transaction gate (GenServer).
//
// HAND-OWNED (gen-app emits a scaffold once, then skips without --force).
//
// NmCfgGate is the OWNER of the config transaction: it has the request payload
// (handle_call), validates it, computes the new NmConfig (clone the committed
// one + apply the delta), stashes it in nm_cfg_shared(), post_event()s the
// matching Txn* into the NmCfgTxn FSM (which does the per PutConfig in on_enter),
// and arms the confirm-window timer. It returns immediately with the txn_state —
// PENDING means "applied, now Confirm over the new path within the window or it
// auto-rolls-back". The statem-never-off-the-wire split: the gate takes the ops
// off TIPC; the FSM owns the state + the per I/O.

#include "lib/NmCfgGate.hh"
#include "impl/nm_cfg_shared.hpp"
#include "lib/NmCfgTxn.hh"           // post_event needs the full FSM type

#include <cstdio>
#include <cstring>
#include <string>

namespace ara::nm {

// ---- process-global shared state (declared in nm_cfg_shared.hpp) -----------
NmCfgShared& nm_cfg_shared() {
    static NmCfgShared s;
    return s;
}

namespace {

// NmCfgTxnState ordinals (mirror lib/NmCfgTxn.hh's enum) for the reply's
// txn_state field — STEADY=0, VALIDATING=1, PENDING=2.
constexpr uint32_t kTxnSteady  = 0;
constexpr uint32_t kTxnPending = 2;

// Post an FSM event if the txn peer is wired (mirrors NmPoller's post_edge).
template <typename Evt>
bool post_txn(Evt evt) {
    auto& ref = nm_cfg_txn_ref();
    if (!ref.valid()) return false;
    ::theia::runtime::post_event(ref.target(), std::move(evt));
    return true;
}

NmCfgReply reply(bool ok, const std::string& msg, const NmCfgShared& sh) {
    NmCfgReply r = system_services_nm_NmCfgReply_init_zero;
    r.ok = ok;
    std::snprintf(r.message, sizeof(r.message), "%s", msg.c_str());
    r.profiles  = sh.pending.wifi_profiles_count;
    r.txn_state = sh.txn_pending ? kTxnPending : kTxnSteady;
    return r;
}

// Start a transaction from the committed baseline: pending = committed, ready to
// take the delta. (committed is seeded from per by init(); see below.)
void begin_txn(NmCfgShared& sh) {
    sh.pending = sh.committed;
}

}  // namespace

// init: seed the committed baseline. per re-pushes the stored NmConfig as a
// ConfigUpdated cast on boot (NmPoller/NmDaemon apply it); the gate mirrors that
// baseline lazily — on the first op it clones whatever it last saw. We start
// from init_zero (defaults) and let the first AddWifi build on it; a future
// refinement can GetConfig from per here to pre-seed. Keep it simple + correct:
// the committed view converges once an op runs.
void NmCfgGate::init(NmCfgGateState& /*s*/) {
    auto& sh = nm_cfg_shared();
    sh.committed = system_services_nm_NmConfig_init_zero;
    sh.committed_known = false;
    sh.txn_pending = false;
}

void NmCfgGate::handle_info(const char* /*info*/, NmCfgGateState& /*s*/) {}

// AddWifi — enroll (or update) a wifi profile, priority and all. Idempotent on
// ssid: a repeat updates psk/priority in place.
NmCfgReply NmCfgGate::handle_call(const AddWifiReq& req, NmCfgGateState& /*s*/) {
    auto& sh = nm_cfg_shared();
    if (sh.txn_pending)
        return reply(false, "a config change is PENDING — confirm/abort first", sh);
    if (req.ssid[0] == '\0')
        return reply(false, "ssid is empty", sh);

    begin_txn(sh);
    auto& cfg = sh.pending;
    // find-or-append by ssid (idempotent update).
    int idx = -1;
    for (pb_size_t i = 0; i < cfg.wifi_profiles_count; ++i)
        if (std::strcmp(cfg.wifi_profiles[i].ssid, req.ssid) == 0) { idx = static_cast<int>(i); break; }
    if (idx < 0) {
        if (cfg.wifi_profiles_count >= 8)
            return reply(false, "wifi_profiles full (max 8)", sh);
        idx = static_cast<int>(cfg.wifi_profiles_count++);
    }
    auto& p = cfg.wifi_profiles[idx];
    std::snprintf(p.ssid, sizeof(p.ssid), "%s", req.ssid);
    std::snprintf(p.psk,  sizeof(p.psk),  "%s", req.psk);
    p.priority = req.priority;
    // Enrolling a profile NM should act on implies auto_connect on.
    cfg.auto_connect = true;

    sh.txn_pending = true;
    post_txn(TxnAddWifi{});   // → FSM PENDING → on_enter PutConfig's sh.pending
    char m[128];
    std::snprintf(m, sizeof(m), "enrolled '%s' prio=%u — PENDING, confirm to keep",
                  req.ssid, req.priority);
    log().info(m);
    return reply(true, m, sh);
}

NmCfgReply NmCfgGate::handle_call(const RemoveWifiReq& req, NmCfgGateState& /*s*/) {
    auto& sh = nm_cfg_shared();
    if (sh.txn_pending)
        return reply(false, "a config change is PENDING — confirm/abort first", sh);
    begin_txn(sh);
    auto& cfg = sh.pending;
    int idx = -1;
    for (pb_size_t i = 0; i < cfg.wifi_profiles_count; ++i)
        if (std::strcmp(cfg.wifi_profiles[i].ssid, req.ssid) == 0) { idx = static_cast<int>(i); break; }
    if (idx < 0)
        return reply(false, std::string("no profile '") + req.ssid + "'", sh);
    // compact: shift the tail down one.
    for (pb_size_t i = static_cast<pb_size_t>(idx); i + 1 < cfg.wifi_profiles_count; ++i)
        cfg.wifi_profiles[i] = cfg.wifi_profiles[i + 1];
    cfg.wifi_profiles_count--;
    sh.txn_pending = true;
    post_txn(TxnRemoveWifi{});
    return reply(true, std::string("removed '") + req.ssid + "' — PENDING", sh);
}

// SetVpn — the global VPN policy (require_vpn + auto_vpn). This is the
// connectivity-risky change the two-phase commit exists for.
NmCfgReply NmCfgGate::handle_call(const SetVpnReq& req, NmCfgGateState& /*s*/) {
    auto& sh = nm_cfg_shared();
    if (sh.txn_pending)
        return reply(false, "a config change is PENDING — confirm/abort first", sh);
    begin_txn(sh);
    sh.pending.require_vpn = req.require_vpn;
    sh.pending.auto_vpn    = req.auto_vpn;
    sh.txn_pending = true;
    post_txn(TxnSetVpn{});
    char m[128];
    std::snprintf(m, sizeof(m), "vpn require=%d auto=%d — PENDING, confirm to keep",
                  req.require_vpn, req.auto_vpn);
    log().info(m);
    return reply(true, m, sh);
}

NmCfgReply NmCfgGate::handle_call(const SetAutoConnectReq& req, NmCfgGateState& /*s*/) {
    auto& sh = nm_cfg_shared();
    if (sh.txn_pending)
        return reply(false, "a config change is PENDING — confirm/abort first", sh);
    begin_txn(sh);
    sh.pending.auto_connect = req.auto_connect;
    sh.txn_pending = true;
    post_txn(TxnSetAutoConn{});
    return reply(true, std::string("auto_connect=") +
                 (req.auto_connect ? "true" : "false") + " — PENDING", sh);
}

// ConfirmConfig — the operator re-connected over the new path: COMMIT. The
// pending config is already persisted (on_enter Put it on apply); confirm locks
// it in as the new committed baseline and cancels the rollback timer.
NmCfgReply NmCfgGate::handle_call(const ConfirmReq& /*req*/, NmCfgGateState& /*s*/) {
    auto& sh = nm_cfg_shared();
    if (!sh.txn_pending)
        return reply(false, "nothing pending to confirm", sh);
    sh.committed = sh.pending;
    sh.committed_known = true;
    sh.txn_pending = false;
    post_txn(TxnConfirm{});
    log().info("config CONFIRMED — committed");
    return reply(true, "committed", sh);
}

// AbortConfig — discard the pending change: ROLLBACK now (re-apply committed).
NmCfgReply NmCfgGate::handle_call(const AbortReq& /*req*/, NmCfgGateState& /*s*/) {
    auto& sh = nm_cfg_shared();
    if (!sh.txn_pending)
        return reply(false, "nothing pending to abort", sh);
    sh.pending = sh.committed;   // on_enter(STEADY via TxnAbort) re-applies this
    sh.txn_pending = false;
    post_txn(TxnAbort{});
    log().info("config ABORTED — rolled back to committed");
    return reply(true, "rolled back", sh);
}

}  // namespace ara::nm

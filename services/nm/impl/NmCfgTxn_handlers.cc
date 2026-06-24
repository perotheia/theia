// User handler bodies for NmCfgTxn (STATEM variant) — the config-transaction FSM.
//
// HAND-OWNED (gen-app emits a scaffold once, then skips without --force).
//
// NmCfgTxn owns the two-phase commit. NmCfgGate (which has the request payload)
// computes the new NmConfig, stashes it in nm_cfg_shared(), and post_event()s
// the Txn* event here; the FSM's transition table (lib .hh, from the .art) moves
// STEADY⇄PENDING, and THIS on_enter does the side effect:
//   on_enter(PENDING) → PutConfig sh.pending to per (per pushes ConfigUpdated →
//                        NmPoller/NmDaemon reconfigure LIVE; the single apply path).
//   on_enter(STEADY)  → committed: nothing (already persisted on the PENDING
//                        apply) OR rollback (re-Put sh.committed) when we got here
//                        via TxnAbort/TxnTimeout. We can't see the event in
//                        on_enter, so the gate keeps sh.pending == sh.committed
//                        for a rollback and == the new config for a confirm; we
//                        always Put sh.pending on entering STEADY-from-PENDING,
//                        which is the committed config either way. Idempotent.
//
// per is reached by a runtime RemoteRef to PerClient (tipc 0x80010007) — the
// per_link pattern; no cross-package client stub.

#include "lib/NmCfgTxn.hh"
#include "impl/nm_cfg_shared.hpp"

#include "NodeRef.hh"                         // LocalRef + RemoteRef + call()
#include "TipcMux.hh"                         // own reply pump for the per call
#include "RemoteCodec.hh"                     // THEIA_DECLARE_REMOTE_CODEC
#include "system/services/per/per.pb.h"      // PutConfigReq / PerReply nanopb

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

// The two per types NmCfgTxn sends/receives over TIPC. Declared locally (not via
// per's per_codecs.hh) to avoid a header-path collision with nm's own lib/.
THEIA_DECLARE_REMOTE_CODEC(system_services_per_PutConfigReq)
THEIA_DECLARE_REMOTE_CODEC(system_services_per_PerReply)

namespace ara::nm {

namespace {

// per's PerClient node (Get/Put/Watch config). See system.services.per.
constexpr uint32_t kPerClientType     = 0x80010007u;
constexpr uint32_t kPerClientInstance = 0u;
struct PerClientTag { static constexpr const char* kNodeName = "per_client"; };
using PerRef = ::theia::runtime::RemoteRef<PerClientTag,
                                           kPerClientType, kPerClientInstance>;

// Singleton link to per's PerClient — its OWN TipcMux reply pump, lazily
// connected. A bare RemoteRef + call() has NO reply path (the call times out
// "no reply") unless its fd is watched by a running mux; this mirrors sm's
// SmSupLink / com's per_link. One mutex serializes the blocking call.
struct PerLink {
    PerRef                  ref;
    theia::runtime::TipcMux mux;
    bool                    started = false;
    std::mutex              mu;

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

// PutConfig the given NmConfig to per. target_node = the WORKER name the config
// is keyed under in etcd (/theia/config/<node>); NmConfig is bound by NmDaemon +
// NmPoller — we key it under "nm_daemon". Best-effort: per momentarily
// unreachable → log + the next op retries.
bool put_nmconfig(const system_services_nm_NmConfig& cfg, const char* who) {
    system_services_per_PutConfigReq req = system_services_per_PutConfigReq_init_zero;
    std::snprintf(req.target_node, sizeof(req.target_node), "%s", "nm_daemon");
    pb_ostream_t os = pb_ostream_from_buffer(req.config.bytes, sizeof(req.config.bytes));
    if (!pb_encode(&os, system_services_nm_NmConfig_fields, &cfg)) {
        std::fprintf(stderr, "[nm_cfg_txn] %s: NmConfig encode failed\n", who);
        return false;
    }
    req.config.size = static_cast<pb_size_t>(os.bytes_written);
    req.expect_rev = 0;   // unconditional (no CAS)

    auto& link = PerLink::instance();
    std::lock_guard<std::mutex> lk(link.mu);
    if (!link.ensure_started()) {
        std::fprintf(stderr, "[nm_cfg_txn] %s: PerClient unreachable (connect failed)\n", who);
        return false;
    }
    auto result = ::theia::runtime::call<system_services_per_PerReply>(
        link.ref, req, /*act=*/0, /*timeout_ms=*/3000);
    if (result.tag != ::theia::runtime::CallTag::Reply) {
        std::fprintf(stderr, "[nm_cfg_txn] %s: PutConfig no reply (per down?)\n", who);
        return false;
    }
    if (result.reply.status != 0) {
        std::fprintf(stderr, "[nm_cfg_txn] %s: PutConfig status=%d %s\n",
                     who, static_cast<int>(result.reply.status), result.reply.message);
        return false;
    }
    return true;
}

}  // namespace

// The LocalRef the gate post_events through; published on first FSM entry.
::theia::runtime::LocalRef<NmCfgTxn>& nm_cfg_txn_ref() {
    static ::theia::runtime::LocalRef<NmCfgTxn> ref;
    return ref;
}

// on_enter — runs on the FSM thread after every committed transition (and once
// at init, new==old==STEADY). Publishes self into the LocalRef on first entry,
// then does the per I/O for PENDING (apply) / STEADY (commit-or-rollback).
void NmCfgTxn::on_enter(NmCfgTxnState new_s,
                        NmCfgTxnState old_s,
                        NmCfgTxnData& d) {
    if (!nm_cfg_txn_ref().valid())         // wire the gate→FSM path on first entry
        nm_cfg_txn_ref() = ::theia::runtime::LocalRef<NmCfgTxn>(*this);

    auto& sh = nm_cfg_shared();

    // Mirror the current transaction snapshot into the FSM data term so it rides
    // the STATEM trace (the observer decodes it; rf-theia `Assert Statem Data`
    // checks it). committed/pending are the rollback target + the applied config;
    // confirm_left_s is non-zero only in PENDING (the live confirm window);
    // `note` is the human transition tag (PENDING=apply, STEADY=settle, init).
    d.has_committed = sh.committed_known;
    d.committed     = sh.committed;
    d.has_pending   = true;
    d.pending       = sh.pending;

    if (new_s == NmCfgTxnState::PENDING) {
        d.confirm_left_s = kConfirmWindowMs / 1000;
        std::snprintf(d.note, sizeof(d.note), "apply");
        // Applied-but-unconfirmed: persist the pending config so per pushes it
        // live (NmPoller reconfigures: associate the new wifi / flip the VPN).
        if (put_nmconfig(sh.pending, "apply"))
            log().info("PENDING: applied pending config via per (awaiting confirm)");
        return;
    }

    d.confirm_left_s = 0;   // no confirm window outside PENDING
    if (new_s == NmCfgTxnState::STEADY && old_s == NmCfgTxnState::PENDING) {
        std::snprintf(d.note, sizeof(d.note), "settle");
        // We left PENDING: either Confirm (sh.pending == sh.committed, the gate
        // set committed=pending) or Abort/Timeout (the gate set pending=committed).
        // Either way sh.pending now holds the config to keep — re-Put it so a
        // rollback restores connectivity; a confirm re-Put is a harmless no-op.
        if (put_nmconfig(sh.pending, "settle"))
            log().info("STEADY: settled config via per (commit or rollback)");
        return;
    }
    std::snprintf(d.note, sizeof(d.note), "steady");   // init / STEADY self-entry
}

}  // namespace ara::nm

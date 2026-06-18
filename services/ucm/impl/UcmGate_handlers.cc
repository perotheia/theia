// User handler bodies for UcmGate.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/UcmGate.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/UcmGate.hh"
#include "lib/UcmFsm.hh"        // the agent FSM the gate post_event()s into
#include "impl/release_dir.hpp" // the release-directory model (A/B replacement)

#include "GenStateM.hh"         // theia::runtime::post_event
#include "NodeRef.hh"           // theia::runtime::LocalRef
#include "TimerService.hh"      // send_after / process_timers (verify window)

#include <pb_decode.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ara::ucm {

// ---- IMPL-owned shared singletons (the gate→fsm wiring) -------------------
// The agent FSM peer. UcmFsm::on_enter publishes itself here on first entry;
// UcmDaemon (RequestUpdate) + UcmGate (every lifecycle step) post_event into it.
theia::runtime::LocalRef<UcmFsm>& ucm_fsm_ref() {
    static theia::runtime::LocalRef<UcmFsm> ref;
    return ref;
}

// The in-flight package manifest UcmDaemon accepted. UcmGate reads it to know
// what to download/stage/switch. One update in flight at a time. (Use the
// nanopb C type directly — the PackageManifest alias lives in UcmDaemon.hh, not
// UcmGate's lib, since the gate's ports don't carry it.)
system_services_ucm_PackageManifest& ucm_pending_manifest() {
    static system_services_ucm_PackageManifest m =
        system_services_ucm_PackageManifest_init_zero;
    return m;
}

namespace {
// Post an Ev* into the agent FSM if it's wired (first ticks can race the FSM's
// initial-entry publish — dropping then is harmless; the step re-fires).
template <typename Evt>
void to_fsm(const char* name, Evt evt) {
    auto& ref = ucm_fsm_ref();
    if (!ref.valid()) {
        std::fprintf(stderr, "[ucm_gate] %s before FSM wired — dropping\n", name);
        return;
    }
    theia::runtime::post_event(ref.target(), std::move(evt));
}
}  // namespace

// ---- OTP init/1 — runs once on the node thread after start().
void UcmGate::init(UcmGateState& /*s*/) {
    this->log().info("ucm gate up — release-dir executor + health gate ready");
}

// ---- string handle_info — the verify-window timer lands here. If PHM hasn't
//      aborted (s.verifying still true), the window closed clean → COMMIT
//      (VERIFYING→ACTIVE). A PHM abort already cleared `verifying` + posted
//      EvFailed, so a late timer is a no-op.
void UcmGate::handle_info(const char* info, UcmGateState& s) {
    if (info && std::strcmp(info, "verify_done") == 0 && s.verifying) {
        s.verifying = false;
        this->log().info("verify window clean — committing");
        to_fsm("EvVerified", EvVerified{});   // VERIFYING → ACTIVE
    }
}

// ---- config update — apply the etcd-backed UcmConfig (releases_root / verify
//      budget / retain) live.
void UcmGate::on_config_update(
        const platform_runtime_ConfigUpdated& cfg,
        UcmGateState& s) {
    system_services_ucm_UcmConfig c = system_services_ucm_UcmConfig_init_zero;
    pb_istream_t is = pb_istream_from_buffer(
        reinterpret_cast<const pb_byte_t*>(cfg.config.bytes), cfg.config.size);
    if (!pb_decode(&is, system_services_ucm_UcmConfig_fields, &c)) {
        this->log().warn("on_config_update: UcmConfig decode failed — ignored");
        return;
    }
    if (c.releases_root[0])    s.releases_root    = c.releases_root;
    if (c.verify_budget_ms)    s.verify_budget_ms = c.verify_budget_ms;
    if (c.retain_releases)     s.retain_releases  = c.retain_releases;
    this->log().info(std::string("config: releases_root=") + s.releases_root +
        " verify_budget_ms=" + std::to_string(s.verify_budget_ms) +
        " retain=" + std::to_string(s.retain_releases));
}

namespace {
// Advance the FSM (→ progress broadcast) AND drive the next gate step inline.
// The release-dir ops are fast in-process filesystem calls, so a synchronous
// chain within the gate thread is correct + race-free (no self-posting). On any
// failure a step posts EvFailed → the FSM goes ROLLBACK + the gate rolls back.
}  // namespace

// Each handler runs the work for the state it just ENTERED, then posts the
// trigger for the NEXT transition (the FSM advances + broadcasts UcmProgress).
// The events line up with the .art statem triggers:
//   EvStartUpdate→DOWNLOADED, EvValidated→VALIDATED, EvStaged→STAGED,
//   EvInstalled→INSTALLING, EvRestarted→RESTARTING, EvVerified→VERIFYING/ACTIVE.

// EvStartUpdate (→ DOWNLOADED) — snapshot the manifest + "download" the artifact
// (in this in-process demo the package is pre-staged at artifact_path; a real
// build fetches it here). Then trigger VALIDATED.
void UcmGate::handle_cast(const EvStartUpdate& /*msg*/, UcmGateState& s) {
    const auto& m = ucm_pending_manifest();
    s.version = m.version;
    s.kind    = static_cast<uint32_t>(m.kind);
    s.scope   = static_cast<uint32_t>(m.scope);
    s.verifying = false;
    this->log().info(std::string("update begin: ") + m.name + " v" + m.version +
        " (downloaded)");
    to_fsm("EvValidated", EvValidated{});   // DOWNLOADED → VALIDATED
}

// EvDownloaded — declared on UcmLifecycleIn for completeness, but the .art FSM
// has no transition on it (EvStartUpdate already enters DOWNLOADED + the gate
// downloads inline). A no-op so the receiver-port handler exists.
void UcmGate::handle_cast(const EvDownloaded& /*msg*/, UcmGateState& /*s*/) {
}

// EvValidated (→ VALIDATED) — crypto signature verdict. Fail-CLOSED: a !ok
// verdict must NOT proceed. (The to_crypto Verify call is wired in a follow-up;
// until then an empty signature is dev/unsigned-OK and a set one is accepted
// pending the crypto edge — never silently bypassed.) Then trigger STAGED.
void UcmGate::handle_cast(const EvValidated& /*msg*/, UcmGateState& s) {
    // TODO(crypto edge): call to_crypto Verify(signature); post EvFailed on !ok.
    to_fsm("EvStaged", EvStaged{});         // VALIDATED → STAGED
    (void)s;
}

// EvStaged (→ STAGED) — materialise the release dir under releases/<ver>. Then
// trigger INSTALLING.
void UcmGate::handle_cast(const EvStaged& /*msg*/, UcmGateState& s) {
    ReleaseLayout l(s.releases_root);
    if (!ensure_release_skeleton(l, s.version)) {
        this->log().warn("stage failed — rolling back");
        to_fsm("EvFailed", EvFailed{});
        return;
    }
    this->log().info(std::string("staged release ") + l.release_of(s.version));
    to_fsm("EvInstalled", EvInstalled{});
}

// EvInstalled — the ATOMIC SWITCH. FULL: current→releases/<ver> (previous set
// first). PARTIAL: swap one FC's binary + symlink (supervisor restarts just it
// — the to_sup edge is wired in a later step). Then RESTARTING.
void UcmGate::handle_cast(const EvInstalled& /*msg*/, UcmGateState& s) {
    ReleaseLayout l(s.releases_root);
    const bool full = (s.scope == 0u /*US_FULL*/);
    if (full) {
        if (!switch_full(l, s.version)) {
            this->log().warn("switch failed — rolling back");
            to_fsm("EvFailed", EvFailed{});
            return;
        }
        this->log().info(std::string("switched current → ") + s.version);
    } else {
        // PARTIAL: the per-FC binary swap + supervisor TerminateChild(no_restart)
        // → StartChild is wired with the to_sup edge in a follow-up. The release
        // dir is staged; record intent.
        this->log().info(std::string("partial install for ") + s.version +
            " (per-FC swap via supervisor — to_sup edge follows)");
    }
    to_fsm("EvRestarted", EvRestarted{});
}

// EvRestarted — the FC(s) are back on the new release (FULL: SM restarts FGs;
// PARTIAL: supervisor restarted the one child). Enter the PHM verify window.
void UcmGate::handle_cast(const EvRestarted& /*msg*/, UcmGateState& s) {
    s.verifying = true;
    // Arm the verify-budget timer: if PHM hasn't flagged a failure by then, COMMIT.
    ::theia::runtime::send_after(::theia::runtime::process_timers(),
        s.verify_budget_ms ? s.verify_budget_ms : 30000, *this, "verify_done");
    this->log().info(std::string("verifying (") +
        std::to_string(s.verify_budget_ms) + "ms PHM window)");
    to_fsm("EvVerified", EvVerified{});   // RESTARTING → VERIFYING
}

// EvVerified — fired TWICE in the FSM ladder: RESTARTING→VERIFYING (above) then
// VERIFYING→ACTIVE (the commit, from the verify-budget timer). The gate side
// only commits + prunes when the window closed cleanly.
void UcmGate::handle_cast(const EvVerified& /*msg*/, UcmGateState& s) {
    if (!s.verifying) {
        // The commit pass (timer fired, window clean): prune old releases.
        ReleaseLayout l(s.releases_root);
        prune_releases(l, s.retain_releases ? s.retain_releases : 3);
        this->log().info(std::string("update ACTIVE: v") + s.version +
            " committed; previous retained for rollback");
    }
}

// EvFailed — a step failed OR PHM flagged the FC unhealthy in the window. Roll
// the release back (FULL: current→previous) and report.
void UcmGate::handle_cast(const EvFailed& /*msg*/, UcmGateState& s) {
    s.verifying = false;
    ReleaseLayout l(s.releases_root);
    if (s.scope == 0u /*US_FULL*/) {
        if (rollback_full(l))
            this->log().warn(std::string("ROLLBACK: current → previous (was v") +
                s.version + ")");
        else
            this->log().error("ROLLBACK failed — no previous release!");
    } else {
        this->log().warn("partial rollback (restore prior FC binary — to_sup edge follows)");
    }
    to_fsm("EvRolledBack", EvRolledBack{});
}

void UcmGate::handle_cast(const EvRolledBack& /*msg*/, UcmGateState& /*s*/) {
    this->log().info("rolled back to previous release — agent IDLE");
}

// PHM health gate. A DEGRADED/FAILED verdict DURING the verify window aborts the
// update: post EvFailed → ROLLBACK. Outside the window it's informational.
void UcmGate::handle_cast(const PhmHealthStatus& msg, UcmGateState& s) {
    // level: 0=OK 1=WARNING 2=DEGRADED 3=FAILED (HealthLevel).
    if (s.verifying && msg.level >= 2u) {
        s.verifying = false;
        this->log().warn(std::string("PHM ") +
            (msg.level >= 3u ? "FAILED" : "DEGRADED") + " on " + msg.entity +
            " during verify — aborting update");
        to_fsm("EvFailed", EvFailed{});
    }
}




}  // namespace ara::ucm

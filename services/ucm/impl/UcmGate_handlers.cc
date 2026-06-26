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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ara::ucm {

// ---- IMPL-owned shared singletons (the daemon→gate→fsm wiring) ------------
// The agent FSM peer. UcmFsm::on_enter publishes itself here on first entry;
// the gate post_event's the .art transition triggers into it for the STATE +
// the UcmProgress broadcast.
theia::runtime::LocalRef<UcmFsm>& ucm_fsm_ref() {
    static theia::runtime::LocalRef<UcmFsm> ref;
    return ref;
}

// The gate peer. UcmGate::init publishes itself here. UcmDaemon (RequestUpdate)
// posts EvStartUpdate into the GATE (not the FSM) so the gate does the WORK; the
// gate then advances the FSM (broadcast) AND re-posts the next event to ITSELF
// to drive the chain. Mailbox-posted (cross-thread-safe), so each step runs on
// the gate's own thread without re-entrancy.
theia::runtime::LocalRef<UcmGate>& ucm_gate_ref() {
    static theia::runtime::LocalRef<UcmGate> ref;
    return ref;
}

// The in-flight package manifest UcmDaemon accepted. UcmGate reads it to know
// what to download/stage/switch. One update in flight at a time. (Nanopb C type
// directly — the PackageManifest alias lives in UcmDaemon.hh, not UcmGate's lib.)
system_services_ucm_PackageManifest& ucm_pending_manifest() {
    static system_services_ucm_PackageManifest m =
        system_services_ucm_PackageManifest_init_zero;
    return m;
}

namespace {
// Advance the FSM (→ UcmProgress broadcast for that state).
template <typename Evt>
void to_fsm(const char* name, Evt evt) {
    auto& ref = ucm_fsm_ref();
    if (!ref.valid()) {
        std::fprintf(stderr, "[ucm_gate] %s before FSM wired — dropping\n", name);
        return;
    }
    theia::runtime::post_event(ref.target(), std::move(evt));
}
// Drive the NEXT gate step (enqueue onto the gate's own mailbox — runs after the
// current handler returns, so the chain is a sequence of mailbox steps, not deep
// recursion).
template <typename Evt>
void to_gate(Evt evt) {
    auto& ref = ucm_gate_ref();
    if (ref.valid()) cast(ref, std::move(evt));
}
// Advance BOTH: the FSM (state+broadcast) and the gate (do the next state's work).
template <typename Evt>
void step(const char* name, Evt evt) {
    to_fsm(name, evt);
    to_gate(evt);
}

// ---- The two-phase-commit activation marker -------------------------------
// Persisted OUTSIDE current/ (at <prefix>/.ucm_activation) so it survives BOTH a
// reboot AND the current→releases/<ver> symlink swap. The reboot-surviving record
// of "this version is PROVISIONAL (unconfirmed), roll back if not confirmed by
// deadline". <prefix> = the parent of releases_root (e.g. /opt/theia). Plain text
// (state|version|campaign_id|deadline_ns|scope) — no proto dep, trivially readable.
// (Phase B mirrors this into per/etcd for the cross-board V-UCM aggregate read.)
std::string marker_path(const std::string& releases_root) {
    // releases_root = <prefix>/releases → marker at <prefix>/.ucm_activation
    auto pos = releases_root.find_last_of('/');
    std::string prefix = (pos == std::string::npos) ? "." : releases_root.substr(0, pos);
    return prefix + "/.ucm_activation";
}
struct Marker {
    int         state = 0;   // ActivationState: 0 NONE 1 PROVISIONAL 2 CONFIRMED
    std::string version;
    std::string campaign_id;
    uint64_t    deadline_ns = 0;
    uint32_t    scope = 0;
    bool        valid = false;
};
void write_marker(const std::string& root, const Marker& m) {
    FILE* f = std::fopen(marker_path(root).c_str(), "w");
    if (!f) return;
    std::fprintf(f, "%d|%s|%s|%llu|%u\n", m.state, m.version.c_str(),
                 m.campaign_id.c_str(),
                 static_cast<unsigned long long>(m.deadline_ns), m.scope);
    std::fclose(f);
}
void clear_marker(const std::string& root) { std::remove(marker_path(root).c_str()); }
Marker read_marker(const std::string& root) {
    Marker m;
    FILE* f = std::fopen(marker_path(root).c_str(), "r");
    if (!f) return m;
    char ver[64] = {0}, camp[96] = {0};
    unsigned long long dl = 0; int st = 0; unsigned sc = 0;
    if (std::fscanf(f, "%d|%63[^|]|%95[^|]|%llu|%u", &st, ver, camp, &dl, &sc) >= 1) {
        m.state = st; m.version = ver; m.campaign_id = camp;
        m.deadline_ns = dl; m.scope = sc; m.valid = true;
    }
    std::fclose(f);
    return m;
}
uint64_t now_ns() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}
// Parse a confirm window (ms) out of the manifest `requires` list: an entry
// "confirm=<ms>" means "two-phase: hold PROVISIONAL <ms> for a remote Confirm".
uint32_t parse_confirm_window(const system_services_ucm_PackageManifest& m) {
    for (pb_size_t i = 0; i < m.requires_count; ++i) {
        const char* r = m.requires[i];
        if (std::strncmp(r, "confirm=", 8) == 0)
            return static_cast<uint32_t>(std::strtoul(r + 8, nullptr, 10));
    }
    return 0;
}
}  // namespace

// ---- OTP init/1 — publish self to the gate ref so the daemon can post into us.
void UcmGate::init(UcmGateState& s) {
    if (!ucm_gate_ref().valid())
        ucm_gate_ref() = theia::runtime::LocalRef<UcmGate>(*this);
    this->log().info("ucm gate up — release-dir executor + health gate ready");

    // ---- REBOOT-RESUME (the two-phase commit's durable half) --------------
    // Read the persistent activation marker. If a PROVISIONAL update was in flight
    // when we went down, we are running the UNCONFIRMED N+1 right now — so:
    //   deadline already passed → nobody confirmed in time → ROLL BACK.
    //   still within window      → resume PROVISIONAL + re-arm the remaining time.
    // (A CONFIRMED/absent marker = steady state, nothing to do.)
    Marker m = read_marker(s.releases_root);
    if (m.valid && m.state == 1 /*PROVISIONAL*/) {
        s.version = m.version;
        s.campaign_id = m.campaign_id;
        s.scope = m.scope;
        s.confirm_deadline_ns = m.deadline_ns;
        const uint64_t now = now_ns();
        if (now >= m.deadline_ns) {
            this->log().warn(std::string("BOOT: provisional v") + m.version +
                " unconfirmed past deadline — ROLLING BACK");
            s.provisioning = false;
            // drive the FSM through to ROLLBACK from IDLE: it'll restore previous.
            step("EvFailed", EvFailed{});
        } else {
            uint32_t remain_ms =
                static_cast<uint32_t>((m.deadline_ns - now) / 1000000ull);
            s.provisioning = true;
            this->log().warn(std::string("BOOT: resuming PROVISIONAL v") + m.version +
                " (confirm within " + std::to_string(remain_ms) + "ms or rollback)");
            ::theia::runtime::send_after(::theia::runtime::process_timers(),
                remain_ms ? remain_ms : 1, *this, "confirm_deadline");
            // reflect the provisional state in the FSM/progress for observers.
            to_fsm("EvProvisional", EvProvisional{});
        }
    }
}

// ---- string handle_info — the verify-window timer lands here. If PHM hasn't
//      aborted (s.verifying still true), the window closed clean → COMMIT
//      (VERIFYING→ACTIVE). A PHM abort already cleared `verifying` + posted
//      EvFailed, so a late timer is a no-op.
void UcmGate::handle_info(const char* info, UcmGateState& s) {
    if (info && std::strcmp(info, "verify_done") == 0 && s.verifying) {
        s.verifying = false;
        if (s.confirm_window_ms == 0) {
            // legacy / no confirm required → commit straight to ACTIVE.
            this->log().info("verify window clean — committing");
            to_fsm("EvVerified", EvVerified{});   // VERIFYING → ACTIVE
        } else {
            // TWO-PHASE: hold PROVISIONAL, persist the marker, arm the confirm
            // deadline. A remote Confirm before then → ACTIVE; else auto-rollback.
            s.provisioning = true;
            s.confirm_deadline_ns = now_ns() +
                static_cast<uint64_t>(s.confirm_window_ms) * 1000000ull;
            Marker m{1 /*PROVISIONAL*/, s.version, s.campaign_id,
                     s.confirm_deadline_ns, s.scope, true};
            write_marker(s.releases_root, m);
            ::theia::runtime::send_after(::theia::runtime::process_timers(),
                s.confirm_window_ms, *this, "confirm_deadline");
            this->log().info(std::string("verify clean — PROVISIONAL (awaiting "
                "Confirm, ") + std::to_string(s.confirm_window_ms) + "ms)");
            to_fsm("EvProvisional", EvProvisional{});   // VERIFYING → PROVISIONAL
        }
        return;
    }
    // The confirm deadline fired and no Confirm cleared `provisioning` → roll back.
    if (info && std::strcmp(info, "confirm_deadline") == 0 && s.provisioning) {
        s.provisioning = false;
        this->log().warn("confirm deadline passed — no Confirm; ROLLING BACK");
        step("EvFailed", EvFailed{});   // PROVISIONAL → ROLLBACK
    }
}

// EvProvisional — the gate side of VERIFYING→PROVISIONAL. The state + marker are
// already set in handle_info; this is the FSM-broadcast pass (no extra work).
void UcmGate::handle_cast(const EvProvisional& /*msg*/, UcmGateState& /*s*/) {
}

// EvConfirmed — a remote Confirm (GS/VUCM) arrived within the window. Clear the
// provisional state + marker (now CONFIRMED) and commit: PROVISIONAL → ACTIVE.
void UcmGate::handle_cast(const EvConfirmed& /*msg*/, UcmGateState& s) {
    if (!s.provisioning) {
        this->log().warn("Confirm with no provisional update in flight — ignored");
        return;
    }
    s.provisioning = false;
    clear_marker(s.releases_root);   // committed → drop the rollback marker
    ReleaseLayout l(s.releases_root);
    prune_releases(l, s.retain_releases ? s.retain_releases : 3);
    this->log().info(std::string("CONFIRMED v") + s.version +
        " → ACTIVE (committed; previous retained)");
    to_fsm("EvConfirmed", EvConfirmed{});   // PROVISIONAL → ACTIVE
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
    s.provisioning = false;
    s.confirm_window_ms = parse_confirm_window(m);   // 0 = legacy direct commit
    s.campaign_id = m.name;   // VUCM/Mender pass the campaign id as the manifest name
    this->log().info(std::string("update begin: ") + m.name + " v" + m.version +
        (s.confirm_window_ms ? (" (two-phase, confirm window " +
                                std::to_string(s.confirm_window_ms) + "ms)") : "") +
        " (downloaded)");
    step("EvValidated", EvValidated{});     // DOWNLOADED → VALIDATED (+drive gate)
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
    step("EvStaged", EvStaged{});           // VALIDATED → STAGED (+drive gate)
    (void)s;
}

// EvStaged (→ STAGED) — materialise the release dir under releases/<ver>. Then
// trigger INSTALLING.
void UcmGate::handle_cast(const EvStaged& /*msg*/, UcmGateState& s) {
    ReleaseLayout l(s.releases_root);
    if (!ensure_release_skeleton(l, s.version)) {
        this->log().warn("stage failed — rolling back");
        step("EvFailed", EvFailed{});
        return;
    }
    this->log().info(std::string("staged release ") + l.release_of(s.version));
    step("EvInstalled", EvInstalled{});
}

// EvInstalled — APPLY the update. The path forks on UpdateKind:
//   UK_CONFIG: NO binary swap — populate etcd config (+ props) from the package.
//   UK_SOFTWARE FULL:    current→releases/<ver> atomic symlink switch.
//   UK_SOFTWARE PARTIAL: swap one FC's binary (supervisor restarts just it).
// Then trigger RESTARTING.
void UcmGate::handle_cast(const EvInstalled& /*msg*/, UcmGateState& s) {
    ReleaseLayout l(s.releases_root);

    if (s.kind == 1u /*UK_CONFIG*/) {
        // Configuration Package: push the package's config/ into etcd via per
        // (the SOLE etcd client) + drop static props into the release's config/.
        // Reuses the PREP-B seed path (migration/seed.py defaults → per
        // PutConfig); per fans the change to FCs as ConfigUpdated casts LIVE — no
        // binary switch, no platform restart. A digest bump runs the
        // gen-transform plugin via per MigrateBulk (the migration tooling).
        if (!apply_config_package(l, s.version)) {
            this->log().warn("config apply failed — rolling back");
            step("EvFailed", EvFailed{});
            return;
        }
        this->log().info(std::string("config v") + s.version +
            " applied to etcd (per → ConfigUpdated casts, live)");
        step("EvRestarted", EvRestarted{});
        return;
    }

    // Software Package.
    const bool full = (s.scope == 0u /*US_FULL*/);
    if (full) {
        if (!switch_full(l, s.version)) {
            this->log().warn("switch failed — rolling back");
            step("EvFailed", EvFailed{});
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
    step("EvRestarted", EvRestarted{});
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
    s.provisioning = false;
    clear_marker(s.releases_root);   // a rollback drops the provisional marker
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
        step("EvFailed", EvFailed{});
    }
}




}  // namespace ara::ucm

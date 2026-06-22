// User handler bodies for PhmGate — the PHM ingest gate.
//
// HAND-OWNED (gen-app emits this once, then skips it without --force).
// PhmGate is the only TIPC-reachable PHM surface. It SUBSCRIBES to the four
// AUTOSAR supervision inputs and does the per-entity THRESHOLD arithmetic,
// then post_event()s the resulting level-change into PhmFsm's fault FSM
// IN-PROCESS via a LocalRef<PhmFsm> — so the statem never handles a raw wire
// message (same {Gate; Fsm} split as SM). The four inputs:
//
//   alive    — SupervisionEvent  (child died/restarted; from system.supervisor)
//   alive    — HeartbeatReport    (seq gap == missed beats; supervisor watchdogs
//                                   the hard case, PHM just notes it)
//   deadline — SendTimeoutReport  (an outbound call/cast blew its budget)
//   log/state— PhmCheckpoint      (app-reported out-of-order / illegal step)
//
// The gate decides observe-vs-escalate; the FSM owns OK→WARNING→DEGRADED→
// FAILED and the on_enter escalation casts. PhmConfig thresholds are loaded
// at init and refreshed on a ConfigUpdated cast (on_config_update).

#include "lib/PhmGate.hh"
#include "lib/PhmFsm.hh"   // the FSM we post_event into

#include "GenStateM.hh"    // theia::runtime::post_event
#include "NodeRef.hh"      // theia::runtime::LocalRef

#include <pb_decode.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>

namespace ara::phm {

// Shared LocalRef<PhmFsm>, set by PhmFsm::on_enter on its first (initial OK)
// entry — wired before any wire event can be forwarded. PhmFsm_handlers.cc
// defines it; declared here for the gate's post_event path. Same publish-on-
// first-entry idiom as SM's fg_statem_ref / sm_statem_ref.
theia::runtime::LocalRef<PhmFsm>& phm_fsm_ref();

namespace {

uint64_t now_ns_() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Forward a fault event into the FSM if the statem peer is wired yet.
template <typename Evt>
void forward_to_fsm(const char* node, const char* name, Evt evt) {
    auto& ref = phm_fsm_ref();
    if (!ref.valid()) {
        std::fprintf(stderr,
            "[%s] %s arrived before PhmFsm wired — dropping\n", node, name);
        return;
    }
    theia::runtime::post_event(ref.target(), std::move(evt));
}

}  // namespace

// ---- OTP init/1 — passive ingest. Seed the thresholds from the .art
//      PhmConfig defaults (per casts a ConfigUpdated when etcd changes them).
void PhmGate::init(PhmGateState& s) {
    // Reset to zero, then seed the non-default thresholds. Use value-init `= {}`
    // (a valid assignment) rather than `= ..._init_zero` (a brace-INIT list): the
    // nanopb macro is fine as a declaration initializer but assigning it to an
    // already-constructed struct is rejected by stricter toolchains (the rpi4
    // aarch64-linux-gnu-g++ does, the host g++ tolerates it). The state member is
    // already _init_zero at construction (PhmGate_state.hh), so this is just an
    // explicit re-seed for a reused state.
    s.config = {};
    s.config.restart_window_ms = 30000;
    s.config.restart_warn      = 1;
    s.config.restart_degrade   = 3;
    s.config.restart_fail      = 5;
    s.config.deadline_degrade  = 5;
    s.config.checkpoint_level  = 2;
}

void PhmGate::handle_info(const char* /*info*/, PhmGateState& /*s*/) {
}

// ---- config update — services/per casts ConfigUpdated when this node's
//      etcd-backed `config PhmConfig` changes. Decode the new thresholds and
//      store them so subsequent fault arithmetic uses the live window/counts.
void PhmGate::on_config_update(
        const platform_runtime_ConfigUpdated& cfg,
        PhmGateState& s) {
    system_services_phm_PhmConfig next =
        system_services_phm_PhmConfig_init_zero;
    pb_istream_t is = pb_istream_from_buffer(cfg.config.bytes, cfg.config.size);
    if (pb_decode(&is, system_services_phm_PhmConfig_fields, &next)) {
        s.config = next;
        this->log().info("PhmConfig updated: window_ms=" +
            std::to_string(s.config.restart_window_ms) + " degrade=" +
            std::to_string(s.config.restart_degrade) + " fail=" +
            std::to_string(s.config.restart_fail) + " entity_overrides=" +
            std::to_string(s.config.entity_policies_count));
    } else {
        this->log().warn("ConfigUpdated: PhmConfig decode failed; keeping prior");
    }
}

// ---- window helper: open/reset the rolling window for an entity when the
//      current one has aged out. Returns the (live) tracker reference.
namespace {
EntityFault& tracker_for(PhmGateState& s, const std::string& entity,
                         uint64_t now, uint64_t window_ns) {
    auto& e = s.entities[entity];
    if (e.window_start_ns == 0 ||
        (window_ns > 0 && now - e.window_start_ns > window_ns)) {
        // Open a fresh window: the prior counts aged out clean.
        e.window_start_ns = now;
        e.restart_count   = 0;
        e.deadline_count  = 0;
    }
    return e;
}

// The thresholds in effect for ONE entity: the PhmConfig global defaults, with
// any PhmEntityPolicy override for this entity applied field-by-field (a zero
// override field inherits the global — so a policy overrides only what differs).
// `fg` defaults to 0 (Machine) unless the override names a Function Group.
struct Thresholds {
    uint32_t restart_warn;
    uint32_t restart_degrade;
    uint32_t restart_fail;
    uint32_t deadline_degrade;
    uint32_t checkpoint_level;
    uint32_t fg;
};

Thresholds effective_thresholds(const PhmGateState& s, const std::string& entity) {
    const auto& g = s.config;
    Thresholds t{ g.restart_warn, g.restart_degrade, g.restart_fail,
                  g.deadline_degrade, g.checkpoint_level, 0 };
    // Linear scan of the override table (≤32 entries, max_count); exact name
    // match. nanopb stores the repeated message as a fixed array + _count.
    for (pb_size_t i = 0; i < g.entity_policies_count; ++i) {
        const auto& p = g.entity_policies[i];
        if (entity != p.entity) continue;
        if (p.restart_warn)     t.restart_warn     = p.restart_warn;
        if (p.restart_degrade)  t.restart_degrade  = p.restart_degrade;
        if (p.restart_fail)     t.restart_fail     = p.restart_fail;
        if (p.deadline_degrade) t.deadline_degrade = p.deadline_degrade;
        if (p.checkpoint_level) t.checkpoint_level = p.checkpoint_level;
        t.fg = p.fg;
        break;
    }
    return t;
}
}  // namespace

// ---- alive: SupervisionEvent (child started/exited/restart). -----------
//
// kind discriminates (supervisor package.art): 0=child_started, 1=child_exited,
// 2=restart_cascade, 3=tombstone_written, 4=escalation, 5=tree_changed. A
// child_exited (1) or restart_cascade (2) is a restart-thrash tick: bump the
// entity's restart_count in the window, then decide observe-vs-escalate against
// the configured thresholds and drive the FSM.
void PhmGate::handle_cast(const SupervisionEvent& msg, PhmGateState& s) {
    const std::string entity = msg.child_name;
    if (entity.empty()) return;

    // Only exit / restart kinds count toward thrash. started/tombstone/
    // escalation/tree_changed are informational for v1.
    const bool is_restart = (msg.kind == 1u || msg.kind == 2u);
    if (!is_restart) return;

    const uint64_t now = now_ns_();
    const uint64_t window_ns =
        static_cast<uint64_t>(s.config.restart_window_ms) * 1000000ull;
    EntityFault& e = tracker_for(s, entity, now, window_ns);
    e.restart_count += 1;

    // Per-entity thresholds (custom policy) — falls back to the global defaults.
    const Thresholds th = effective_thresholds(s, entity);

    std::fprintf(stderr,
        "[%s] SupervisionEvent kind=%u entity=%s restarts=%u/%u(degrade) "
        "%u(fail)\n", kNodeName, msg.kind, entity.c_str(), e.restart_count,
        th.restart_degrade, th.restart_fail);

    // FAILED budget blown → escalate twice (drive OK/WARNING/DEGRADED→FAILED).
    if (e.restart_count >= th.restart_fail) {
        e.level = 3;  // FAILED
        forward_to_fsm(kNodeName, "FaultEscalate(fail)", FaultEscalate{});
        forward_to_fsm(kNodeName, "FaultEscalate(fail)", FaultEscalate{});
    } else if (e.restart_count >= th.restart_degrade) {
        e.level = 2;  // DEGRADED
        forward_to_fsm(kNodeName, "FaultEscalate(degrade)", FaultEscalate{});
    } else if (e.restart_count >= th.restart_warn) {
        if (e.level < 1) e.level = 1;  // WARNING
        forward_to_fsm(kNodeName, "FaultObserved(warn)", FaultObserved{});
    }
}

// ---- alive: HeartbeatReport (seq gap == missed sends). -----------------
//
// The supervisor already watchdogs heartbeats (SIGTERMs nodes that miss K
// deadlines), so PHM's alive signal is primarily SupervisionEvent. For v1 we
// only NOTE a seq gap (a non-contiguous seq) — no escalation, to avoid double-
// counting the supervisor's own watchdog action.
void PhmGate::handle_cast(const HeartbeatReport& msg, PhmGateState& s) {
    const std::string node = msg.node_name;
    if (node.empty()) return;
    EntityFault& e = s.entities[node];
    if (e.have_hb_seq && msg.seq > e.last_hb_seq + 1) {
        std::fprintf(stderr,
            "[%s] HeartbeatReport gap on %s: seq %llu → %llu (%llu missed)\n",
            kNodeName, node.c_str(),
            (unsigned long long)e.last_hb_seq, (unsigned long long)msg.seq,
            (unsigned long long)(msg.seq - e.last_hb_seq - 1));
    }
    e.last_hb_seq = msg.seq;
    e.have_hb_seq = true;
}

// ---- deadline: SendTimeoutReport (an outbound call/cast blew its budget). --
//
// Bump the caller_node's deadline_count in the window; at deadline_degrade
// crossings escalate. A single miss is a FaultObserved (watch); the threshold
// crossing is a FaultEscalate (→ DEGRADED).
void PhmGate::handle_cast(const SendTimeoutReport& msg, PhmGateState& s) {
    const std::string entity = msg.caller_node;
    if (entity.empty()) return;

    const uint64_t now = now_ns_();
    const uint64_t window_ns =
        static_cast<uint64_t>(s.config.restart_window_ms) * 1000000ull;
    EntityFault& e = tracker_for(s, entity, now, window_ns);
    e.deadline_count += 1;

    // Per-entity thresholds (custom policy) — falls back to the global defaults.
    const Thresholds th = effective_thresholds(s, entity);

    std::fprintf(stderr,
        "[%s] SendTimeoutReport %s→%s %s.%s budget=%ums observed=%ums "
        "count=%u/%u\n", kNodeName, msg.caller_node, msg.callee_node,
        msg.iface, msg.method, msg.budget_ms, msg.observed_ms,
        e.deadline_count, th.deadline_degrade);

    if (e.deadline_count >= th.deadline_degrade) {
        e.level = 2;  // DEGRADED
        forward_to_fsm(kNodeName, "FaultEscalate(deadline)", FaultEscalate{});
    } else {
        if (e.level < 1) e.level = 1;
        forward_to_fsm(kNodeName, "FaultObserved(deadline)", FaultObserved{});
    }
}

// ---- logical/state: PhmCheckpoint (app-reported execution step / lifecycle).
//
// An app reports it reached a named checkpoint; `violation` = the app/PHM
// judged it out-of-order (logical) or an illegal transition (state). A
// violation escalates immediately (an illegal transition is never just a
// warning — checkpoint_level defaults to DEGRADED=2). A clean checkpoint is
// noted only.
void PhmGate::handle_cast(const PhmCheckpoint& msg, PhmGateState& s) {
    const std::string entity = msg.entity;
    if (entity.empty()) return;

    if (!msg.violation) {
        std::fprintf(stderr, "[%s] PhmCheckpoint ok entity=%s checkpoint=%s\n",
                     kNodeName, entity.c_str(), msg.checkpoint);
        return;
    }

    const uint64_t now = now_ns_();
    const uint64_t window_ns =
        static_cast<uint64_t>(s.config.restart_window_ms) * 1000000ull;
    EntityFault& e = tracker_for(s, entity, now, window_ns);
    // Per-entity checkpoint_level (custom policy) — falls back to the global.
    const Thresholds th = effective_thresholds(s, entity);
    e.level = (th.checkpoint_level >= 2u) ? 2u : 1u;

    std::fprintf(stderr,
        "[%s] PhmCheckpoint VIOLATION entity=%s checkpoint=%s kind=%u → escalate\n",
        kNodeName, entity.c_str(), msg.checkpoint, msg.kind);
    forward_to_fsm(kNodeName, "FaultEscalate(checkpoint)", FaultEscalate{});
}

// ---- GetHealthStatus — the ara::phm read surface. Summarise the per-entity
//      table: worst level seen, total tracked, how many are DEGRADED+.
PhmStatusMsg PhmGate::handle_call(
        const PhmStatusReq& /*req*/,
        PhmGateState& s) {
    PhmStatusMsg out = system_services_phm_PhmStatusMsg_init_zero;
    uint32_t worst = 0;
    uint32_t n_degraded = 0;
    for (const auto& kv : s.entities) {
        if (kv.second.level > worst) worst = kv.second.level;
        if (kv.second.level >= 2u) ++n_degraded;
    }
    out.worst =
        static_cast<system_services_phm_HealthLevel>(worst);
    out.n_entities = static_cast<uint32_t>(s.entities.size());
    out.n_degraded = n_degraded;
    out.ts_ns = now_ns_();
    return out;
}

}  // namespace ara::phm

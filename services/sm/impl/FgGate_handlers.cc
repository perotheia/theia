// User handler bodies for FgGate — the MULTI-FG authority.
//
// HAND-OWNED (regenerated only with --force). FgGate is the only TIPC-reachable
// surface for FG events: it receives FgLifecycleIn events (each carrying its
// target `fg`) + the PHM health stream, and now OWNS the per-FG lifecycle —
// tracking each Function Group's FgState in a map and DRIVING that FG's mapped
// supervision sub-tree via the supervisor (sm_sup_link). This realises the
// SM-decides → EM-executes contract PER FUNCTION GROUP (not just one MachineFG):
// PHM degrades FC X (fg=N) → FgGate drops FG N to Restart → supervisor stops
// network_sup/app_sup/… while the OTHER FGs stay up.
//
// The FunctionGroupSm statem node remains as the MachineFG(0) reference (its
// STATEM trace shows the canonical lifecycle); fg=0 events still drive it for
// observability. Every fg (incl. 0) is also tracked + executed here.

#include "lib/FgGate.hh"
#include "lib/FunctionGroupSm.hh"   // the MachineFG reference FSM
#include "impl/sm_sup_link.hpp"     // FgId/fg_subtree + the supervisor RemoteRef

#include "NodeRef.hh"   // theia::runtime::LocalRef

#include <cstdio>
#include <string>
#include <utility>

namespace ara::sm {

// The MachineFG(0) reference FSM, set by FunctionGroupSm::on_enter on first entry.
theia::runtime::LocalRef<FunctionGroupSm>& fg_statem_ref() {
    static theia::runtime::LocalRef<FunctionGroupSm> ref;
    return ref;
}

namespace {

// Mirror an fg=0 event into the MachineFG reference FSM (trace/observability).
template <typename Evt>
void mirror_machine_fg(const char* node, const char* name, Evt evt) {
    auto& ref = fg_statem_ref();
    if (!ref.valid()) {
        std::fprintf(stderr, "[%s] %s before FG statem wired — dropping mirror\n",
                     node, name);
        return;
    }
    theia::runtime::post_event(ref.target(), std::move(evt));
}

// The per-FG transition + EXECUTE. Apply the FgState transition for `fg`, then
// drive its mapped supervision sub-tree (stop on OFF/RESTART/SHUTDOWN-desired,
// start on STARTUP/RUNNING). The supervisor REALISES the desired state; crash-
// recovery stays its own job (stop_subtree uses no_restart so SM's desired-OFF
// doesn't fight an autonomous restart).
void fg_transition(FgGate* self, FgGateState& s, uint32_t fg,
                   uint8_t to_state, const char* ev) {
    s.set_state(fg, to_state);
    const std::string sub = fg_subtree(fg);
    self->log().info(std::string("FG[") + std::to_string(fg) + " " + sub +
        "] " + ev + " → state=" + std::to_string((unsigned)to_state));
    switch (to_state) {
    case 1 /*FG_STARTUP*/:
    case 2 /*FG_RUNNING*/:
        SmSupLink::instance().start_subtree(sub);   // desired-ON
        break;
    case 3 /*FG_SHUTDOWN*/:
    case 4 /*FG_RESTART*/:
        SmSupLink::instance().stop_subtree(sub);    // desired-OFF (no_restart hold)
        break;
    default: /*FG_OFF*/ break;                       // already stopped
    }
}

}  // namespace

void FgGate::init(FgGateState& /*s*/) {
    this->log().info("fg gate up — multi-FG authority (per-FG state + EM drive)");
}

void FgGate::handle_info(const char* /*info*/, FgGateState& /*s*/) {
}

// ---- FgLifecycleIn events — route by msg.fg to that FG's state + sub-tree. --

void FgGate::handle_cast(const FgStart& msg, FgGateState& s) {
    fg_transition(this, s, msg.fg, 1 /*FG_STARTUP*/, "FgStart");
    if (msg.fg == FG_MACHINE) mirror_machine_fg(kNodeName, "FgStart", msg);
}

void FgGate::handle_cast(const FgStarted& msg, FgGateState& s) {
    fg_transition(this, s, msg.fg, 2 /*FG_RUNNING*/, "FgStarted");
    if (msg.fg == FG_MACHINE) mirror_machine_fg(kNodeName, "FgStarted", msg);
}

void FgGate::handle_cast(const FgStop& msg, FgGateState& s) {
    fg_transition(this, s, msg.fg, 3 /*FG_SHUTDOWN*/, "FgStop");
    if (msg.fg == FG_MACHINE) mirror_machine_fg(kNodeName, "FgStop", msg);
}

void FgGate::handle_cast(const FgStopped& msg, FgGateState& s) {
    fg_transition(this, s, msg.fg, 0 /*FG_OFF*/, "FgStopped");
    if (msg.fg == FG_MACHINE) mirror_machine_fg(kNodeName, "FgStopped", msg);
}

void FgGate::handle_cast(const FgDegraded& msg, FgGateState& s) {
    fg_transition(this, s, msg.fg, 4 /*FG_RESTART*/, "FgDegraded");
    if (msg.fg == FG_MACHINE) mirror_machine_fg(kNodeName, "FgDegraded", msg);
}

void FgGate::handle_cast(const FgRetry& msg, FgGateState& s) {
    fg_transition(this, s, msg.fg, 1 /*FG_STARTUP*/, "FgRetry");
    if (msg.fg == FG_MACHINE) mirror_machine_fg(kNodeName, "FgRetry", msg);
}

// ---- PHM health (comm-matrix phm → sm) — TRANSLATE + route to msg.fg. -------
//
// PhmHealthStatus carries { entity, level, kind, fg, detail, ts_ns }. A DEGRADED
// (2) / FAILED (3) verdict drops THAT FG (msg.fg) into Restart — stopping its
// sub-tree while the other FGs stay Running. OK/WARNING are informational.
void FgGate::handle_cast(const PhmHealthStatus& msg, FgGateState& s) {
    const bool critical =
        msg.level == system_services_phm_HealthLevel_HealthLevel_DEGRADED ||
        msg.level == system_services_phm_HealthLevel_HealthLevel_FAILED;
    std::fprintf(stderr,
        "[%s] PhmHealthStatus entity=%s level=%u fg=%u%s\n",
        kNodeName, msg.entity, (unsigned)msg.level, (unsigned)msg.fg,
        critical ? " → FgDegraded" : " (below DEGRADED — no FG action)");
    if (critical) {
        fg_transition(this, s, msg.fg, 4 /*FG_RESTART*/, "PHM-degraded");
        if (msg.fg == FG_MACHINE) {
            FgDegraded fd = system_services_sm_FgDegraded_init_zero;
            fd.fg = msg.fg;
            mirror_machine_fg(kNodeName, "FgDegraded(PHM)", fd);
        }
    }
}

}  // namespace ara::sm

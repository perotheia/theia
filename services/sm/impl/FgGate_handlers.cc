// User handler bodies for FgGate — the Function-Group FSM's external-message
// front (mirrors SmGate for the machine FSM).
//
// FIRST-TIME-ONLY SCAFFOLD (regenerated only with --force). Declarations in
// lib/FgGate.hh. FgGate is the ONLY TIPC-reachable surface for FG FSM events:
// it receives FgLifecycleIn events + the PHM health stream off the wire and
// post_event()s the matching FG event into FunctionGroupSm's FSM IN-PROCESS via
// a LocalRef<FunctionGroupSm> — so the statem itself never handles a raw wire
// message. A critical PhmHealthStatus is TRANSLATED to FgDegraded here (PHM
// informs; the gate decides the FG action).

#include "lib/FgGate.hh"
#include "lib/FunctionGroupSm.hh"   // the FSM we post_event into

#include "NodeRef.hh"   // theia::runtime::LocalRef

#include <cstdio>
#include <utility>

namespace ara::sm {

// Shared LocalRef<FunctionGroupSm>, set by FunctionGroupSm::on_enter on its
// first (initial FG_OFF) entry — wired before any wire event can be forwarded.
// FunctionGroupSm_handlers.cc references it via the same extern declaration.
theia::runtime::LocalRef<FunctionGroupSm>& fg_statem_ref() {
    static theia::runtime::LocalRef<FunctionGroupSm> ref;
    return ref;
}

namespace {

// Forward an FG event into the FSM if the statem peer is wired yet.
template <typename Evt>
void forward_to_fg_statem(const char* node, const char* name, Evt evt) {
    auto& ref = fg_statem_ref();
    if (!ref.valid()) {
        std::fprintf(stderr,
            "[%s] %s arrived before FG statem wired — dropping\n", node, name);
        return;
    }
    std::fprintf(stderr, "[%s] %s → post_event to FunctionGroupSm FSM\n",
                 node, name);
    theia::runtime::post_event(ref.target(), std::move(evt));
}

}  // namespace

// ---- OTP init/1 — passive gate, nothing to bootstrap.
void FgGate::init(FgGateState& /*s*/) {
}

void FgGate::handle_info(const char* /*info*/, FgGateState& /*s*/) {
}

// ---- FgLifecycleIn events — forward verbatim into the FSM. -----------------

void FgGate::handle_cast(const FgStart& msg, FgGateState& /*s*/) {
    forward_to_fg_statem(kNodeName, "FgStart", msg);
}

void FgGate::handle_cast(const FgStarted& msg, FgGateState& /*s*/) {
    forward_to_fg_statem(kNodeName, "FgStarted", msg);
}

void FgGate::handle_cast(const FgStop& msg, FgGateState& /*s*/) {
    forward_to_fg_statem(kNodeName, "FgStop", msg);
}

void FgGate::handle_cast(const FgStopped& msg, FgGateState& /*s*/) {
    forward_to_fg_statem(kNodeName, "FgStopped", msg);
}

void FgGate::handle_cast(const FgDegraded& msg, FgGateState& /*s*/) {
    forward_to_fg_statem(kNodeName, "FgDegraded", msg);
}

void FgGate::handle_cast(const FgRetry& msg, FgGateState& /*s*/) {
    forward_to_fg_statem(kNodeName, "FgRetry", msg);
}

// ---- PHM health (comm-matrix phm → sm) — TRANSLATE, don't forward. ---------
//
// A PHM health verdict is not an FG event; the gate maps it to one. Today the
// placeholder PhmHealthStatus carries no fields, so any health cast is treated
// as a degraded signal (drop the FG into Restart). When the message grows its
// {entity, level, fg} fields, gate ONLY on a DEGRADED/FAILED level and address
// the named fg — until then this is the conservative "health blip → recover".
void FgGate::handle_cast(const PhmHealthStatus& /*msg*/, FgGateState& /*s*/) {
    std::fprintf(stderr,
        "[%s] PhmHealthStatus → FgDegraded (placeholder: any health cast "
        "→ recover; gate on level once the schema is pinned)\n", kNodeName);
    forward_to_fg_statem(kNodeName, "FgDegraded(from PHM)", FgDegraded{});
}

}  // namespace ara::sm

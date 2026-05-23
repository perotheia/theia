// Hand-rolled equivalent of `artheia gen-cpp-stubs` output for
// services/system/sm/package.art. Mirrors what the codegen would
// produce; kept hand-written today because:
//
//   1. SM messages are not yet proto-encoded (no .pb.h files for
//      SystemBoot / StartupComplete / etc.). When SM gets a proto
//      schema, the codegen path takes over and this file goes away.
//   2. Forces this file into lockstep with package.art — the
//      package.art's statem block IS the spec; this is just C++
//      shorthand. Keep them aligned manually until codegen is wired.
//
// Regeneration check: after editing services/system/sm/package.art,
// run `artheia gen-cpp-stubs services/system/sm/package.art --out /tmp/sm_gen`
// and diff the output against this header to confirm structural
// equivalence (the codegen's enum class is named SmDaemonState, the
// hand-rolled one declares the same enum + uses sm_messages.hh's
// SmStateMsg as data; the transition table is the same).

#pragma once

#include "GenStateM.hh"
#include "sm/sm_messages.hh"

namespace system_services_sm {

class SmDaemon;   // forward — GenStateM<SmDaemon, ...> below

// One-per-node state enum (matches package.art's `states [...]`).
// Identical values to sm_messages.hh::SmState; we use that one as the
// public SmState type and re-aliase here so the codegen-style naming
// works inside the FSM template.
using SmDaemonState = SmState;

// SmDaemonStateMBase — generated structural skeleton.
//
// Inherit and override per-event handle_event for guards, override
// on_enter for side effects (broadcast / persist / log). on_enter
// returns void and so cannot transition — it's safe to call cast()
// from there.
class SmDaemonStateMBase
    : public demo::runtime::GenStateM<SmDaemon, SmDaemonState, SmStateMsg> {
public:
    SmDaemonState init(SmStateMsg& /*d*/) {
        return SmDaemonState::OFF;
    }

    // Keep base-class handle_event(StateTimeoutMsg) visible — derived
    // overloads otherwise hide it (C++ name hiding).
    using demo::runtime::GenStateM<SmDaemon,
                                    SmDaemonState,
                                    SmStateMsg>::handle_event;

    // OFF + SystemBoot → STARTING after 30s
    demo::runtime::EventResult<SmDaemonState> handle_event(
            SmDaemonState s, const SystemBoot& /*e*/, SmStateMsg& /*d*/) {
        if (s == SmDaemonState::OFF) {
            return demo::runtime::transition_to<SmDaemonState>(
                SmDaemonState::STARTING, 30'000);
        }
        return demo::runtime::keep_state<SmDaemonState>();
    }

    // STARTING + StartupComplete → RUNNING
    demo::runtime::EventResult<SmDaemonState> handle_event(
            SmDaemonState s, const StartupComplete& /*e*/, SmStateMsg& /*d*/) {
        if (s == SmDaemonState::STARTING) {
            return demo::runtime::transition_to<SmDaemonState>(
                SmDaemonState::RUNNING);
        }
        return demo::runtime::keep_state<SmDaemonState>();
    }

    // RUNNING + ShutdownRequest → SHUTDOWN
    demo::runtime::EventResult<SmDaemonState> handle_event(
            SmDaemonState s, const ShutdownRequest& /*e*/, SmStateMsg& /*d*/) {
        if (s == SmDaemonState::RUNNING) {
            return demo::runtime::transition_to<SmDaemonState>(
                SmDaemonState::SHUTDOWN);
        }
        return demo::runtime::keep_state<SmDaemonState>();
    }

    // RUNNING + UpdateRequest → UPDATE
    demo::runtime::EventResult<SmDaemonState> handle_event(
            SmDaemonState s, const UpdateRequest& /*e*/, SmStateMsg& /*d*/) {
        if (s == SmDaemonState::RUNNING) {
            return demo::runtime::transition_to<SmDaemonState>(
                SmDaemonState::UPDATE);
        }
        return demo::runtime::keep_state<SmDaemonState>();
    }

    // UPDATE + UpdateComplete → RUNNING
    demo::runtime::EventResult<SmDaemonState> handle_event(
            SmDaemonState s, const UpdateComplete& /*e*/, SmStateMsg& /*d*/) {
        if (s == SmDaemonState::UPDATE) {
            return demo::runtime::transition_to<SmDaemonState>(
                SmDaemonState::RUNNING);
        }
        return demo::runtime::keep_state<SmDaemonState>();
    }

    // DEGRADED + RetryStartup → STARTING after 30s
    demo::runtime::EventResult<SmDaemonState> handle_event(
            SmDaemonState s, const RetryStartup& /*e*/, SmStateMsg& /*d*/) {
        if (s == SmDaemonState::DEGRADED) {
            return demo::runtime::transition_to<SmDaemonState>(
                SmDaemonState::STARTING, 30'000);
        }
        return demo::runtime::keep_state<SmDaemonState>();
    }

    // SHUTDOWN + PowerOff → halt (clean exit)
    demo::runtime::EventResult<SmDaemonState> handle_event(
            SmDaemonState s, const PowerOff& /*e*/, SmStateMsg& /*d*/) {
        if (s == SmDaemonState::SHUTDOWN) {
            return demo::runtime::halt<SmDaemonState>();
        }
        return demo::runtime::keep_state<SmDaemonState>();
    }

    // STARTING — state timeout → DEGRADED
    demo::runtime::EventResult<SmDaemonState> handle_event(
            SmDaemonState s,
            const demo::runtime::StateTimeoutMsg<SmDaemonState>& /*e*/,
            SmStateMsg& /*d*/) {
        switch (s) {
        case SmDaemonState::STARTING:
            return demo::runtime::transition_to<SmDaemonState>(
                SmDaemonState::DEGRADED);
        default:
            return demo::runtime::keep_state<SmDaemonState>();
        }
    }
};

}  // namespace system_services_sm

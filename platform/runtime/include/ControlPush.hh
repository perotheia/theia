// theia::runtime — shared apply-logic for the supervisor's control-service
// pushes (LogLevelPush / TraceControlPush).
//
// Both GenServer (mailbox, runs on the node thread) and GenRunnable (no
// mailbox, runs INLINE on the TipcMux dispatch thread) receive the SAME two
// control casts from the supervisor and must apply them IDENTICALLY. To keep
// one source of truth — instead of the logic living inline in GenServer's
// handle_cast and getting copy-pasted into GenRunnable — both call these free
// functions.
//
// Thread-safety: every operation here is on an atomic (Logger::set_level is
// atomic; Tracer's enable / kind-mask are relaxed atomics), so the functions
// are safe to call from EITHER the node's own thread (GenServer) or the mux
// dispatch thread (GenRunnable) without locking.

#pragma once

#include "Logger.hh"
#include "Tracer.hh"
#include "platform_runtime/runtime.pb.h"  // LogLevelPush / TraceControlPush

#include <string>

namespace theia {
namespace runtime {

// LogLevelPush (#386): the supervisor's live log-level change. The enum is
// ordinal-aligned with LogLevel, so applying it is a static_cast + set_level.
// Flips BOTH the node's own logger and the process logger (the legacy /
// fallback target) so the change takes effect regardless of which a line goes
// through. `node_log` is this node's logger (GenServer/GenRunnable this->log()).
inline void apply_log_level_push(Logger& node_log,
                                 const platform_runtime_LogLevelPush& push) noexcept {
    auto lvl = static_cast<LogLevel>(push.level);
    process_logger().set_level(lvl);
    node_log.set_level(lvl);
    node_log.info(std::string("log level -> ") + log_level_name(lvl) +
                  " (supervisor push)");
}

// TraceControlPush (#403): the supervisor's per-node Tracer kind-filter push.
// The kind filter is a BITMASK over TraceKind ordinals — a node can trace
// several kinds at once; each push ADDS or REMOVES one kind incrementally and
// the mask accumulates. mask==0 is the catch-all sentinel ("all kinds pass").
// `tr` is tracer_for(kNodeName); `node_log` this node's logger.
inline void apply_trace_control_push(Tracer& tr, Logger& node_log,
                                     const platform_runtime_TraceControlPush& push) noexcept {
    auto tk = static_cast<TraceKind>(push.kind);
    const bool catch_all = (tk == TraceKind::Other);
    if (push.enabled) {
        // Master on. kind 0 (Other) is the CATCH-ALL: leave the mask at 0
        // (mask==0 → every kind passes). Setting bit 0 would narrow to ONLY
        // kind Other, dropping Recv/Dispatch/Send — so for kind 0 we CLEAR the
        // mask; a non-zero kind ADDS its bit to whatever is already set.
        tr.enable(true);
        if (catch_all) {
            tr.trace_clear_kinds();   // catch-all: all kinds pass
        } else {
            tr.trace_enable_kind(tk, true);   // accumulate into the mask
        }
    } else if (catch_all) {
        // Disable the WHOLE node (`trace <node> off`): master off + wipe mask.
        tr.enable(false);
        tr.trace_clear_kinds();
    } else {
        // Disable ONE kind. Clear just that bit — other selected kinds keep
        // tracing. Only flip the master OFF if clearing the last bit would
        // leave mask==0 ("all pass" — NOT what the user asked) from a
        // previously-narrow mask.
        uint32_t before = tr.trace_kind_mask();
        tr.trace_enable_kind(tk, false);
        if (before != 0 && tr.trace_kind_mask() == 0) {
            tr.enable(false);   // last narrow kind cleared → node silent
        }
    }
    node_log.info(std::string("trace kind ") +
                  std::to_string(static_cast<int>(push.kind)) + " -> " +
                  (push.enabled ? "ON" : "OFF") + " (supervisor push)");
}

}  // namespace runtime
}  // namespace theia

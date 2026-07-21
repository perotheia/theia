// Process-global Supervisor bridge.
//
// The gen-fc FC has TWO nodes that must share ONE supervision engine:
//   - SupervisorWorker (runnable) constructs the engine + runs its loop.
//   - SupervisorCtl (atomic gen_server) enqueue()/call()s typed control
//     ExecCommands into it and is the source of the EmitSink callbacks.
// Rather than thread a pointer between the two node objects (which main.cc
// constructs independently), they reach the engine through this process-
// global accessor — the same pattern as log[trace]'s TraceHub.
//
// Lifetime: SupervisorWorker::do_start() calls set_supervisor() with the
// engine it owns (a stack/local object living for the loop's duration);
// do_stop()/loop-exit clears it. supervisor_instance() returns nullptr until
// then, so a control op that races startup is a safe no-op.

#pragma once

#include "runtime.h"

namespace supervisor {

// Set/clear the process-global engine pointer (called by SupervisorWorker).
void set_supervisor(Supervisor* sup);

// The current engine, or nullptr if the worker hasn't constructed it yet
// (or has torn it down). Callers MUST null-check.
Supervisor* supervisor_instance();

// The control node installs an EmitSink forwarder here (its handlers know how
// to encode + broadcast each event over the `events` senders). The engine —
// constructed by the worker — calls set_emit_sink() with lambdas that defer
// to whatever forwarder is registered, so node construction order doesn't
// matter: an event emitted before the control node is up is a quiet no-op.
// One slot per event kind; each is best-effort.
struct EmitForwarder {
    void (*on_event)(const EventData&)         = nullptr;
    void (*on_health)(const HealthData&)       = nullptr;
    void (*on_snapshot_begin)(uint64_t, uint64_t) = nullptr;
    void (*on_edge)(const EdgeData&)           = nullptr;
    void (*on_node_state)(const NodeStateData&) = nullptr;
    void (*on_snapshot_end)(uint64_t)          = nullptr;
    // Config push to a child BY NAME (trace/log). The engine (e.g. on restart
    // re-push) asks control to set a child's trace/log config; SupervisorCtl —
    // the runtime-backed node, the ONLY one that may touch Theia transport (the
    // worker runnable is a bare thread) — RESOLVES the child address and CASTS
    // the typed TraceControlPush / LogLevelPush. The engine passes the child
    // NAME + the typed values; resolve+cast live entirely in control.
    void (*set_trace)(const char* /*child*/, uint32_t /*kind*/,
                      bool /*enabled*/) = nullptr;
    void (*set_log_level)(const char* /*child*/, uint32_t /*level*/) = nullptr;
    // OTP pg:monitor — push a group's current membership to ONE watcher. The
    // engine (on join/leave/reap, or the PgWatch CALL) asks control to cast a
    // system_supervisor_PgMembership to {watcher_type, watcher_instance}.
    // members is a flat [t0,i0, t1,i1, …] array of `count` PAIRS (2*count u32s) —
    // a plain C ABI so the EmitForwarder stays capture-free function pointers.
    void (*push_pg_membership)(uint32_t /*watcher_type*/,
                               uint32_t /*watcher_instance*/,
                               const char* /*group_name*/, uint32_t /*group_type*/,
                               const uint32_t* /*members_flat*/,
                               uint32_t /*count*/) = nullptr;
};
void set_emit_forwarder(const EmitForwarder& fwd);
const EmitForwarder& emit_forwarder();

}  // namespace supervisor

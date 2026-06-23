// Process-global Supervisor bridge.
//
// The gen-app FC has TWO nodes that must share ONE supervision engine:
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
    // (no pg push hook — pg delivery is TIPC multicast by the broadcaster; the
    // engine only allocates type+instance in the PgJoin CALL reply.)
};
void set_emit_forwarder(const EmitForwarder& fwd);
const EmitForwarder& emit_forwarder();

}  // namespace supervisor

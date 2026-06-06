# Supervisor: static per-process node config (no heartbeat discovery)

> **DONE (2026-06-04).** Already shipped as part of the supervisor migration +
> executor.json node emission. The supervisor parses a static `nodes[]` block
> per worker from executor.json (`spec.cpp:175`, `NodeInfo` struct in
> `spec.h`), so it knows each process's nodes up front (e.g. `p1` →
> counter/driver/ticker, `sm` → sm_daemon/sm_gate) instead of discovering them
> from heartbeats. Each NodeInfo carries name + reporting + tipc_{type,instance}
> (+ cpu/sched from the affinity task), which the supervisor uses to push trace/
> log config and synthesize the tree snapshot — no runtime discovery needed.

## Original idea (verbatim)

sup now gets child process `[nodes]`; previously it discovered nodes from
heartbeat, now it can configure nodes inside a process in the sup tree
statically.

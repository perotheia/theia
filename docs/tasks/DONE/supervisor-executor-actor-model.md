# Supervisor executor — typed-command actor model (forward rewrite)

## Context

Commit `1e64178` ("supervisor: direct control — drop run_on_engine, lock the
engine") had SupervisorCtl call `eng->ctl_*()` **directly from the TipcMux
thread**, serialized by a coarse `recursive state_mu_`. That reintroduces the
classic single-owner hazards:
- A `ctl_*` call on the TipcMux thread forks/mutates **under the same lock the
  loop's `reap()` takes** → an unrelated control caller can **delay SIGCHLD
  reaping**.
- `config_repush → ctl_set_* → resolve_node` re-enters `state_mu_`, which is why
  it had to be made **recursive** — a smell that the lock is misplaced.

**Do NOT revert to the old `run_on_engine` shape.** The objection that drove
`1e64178` was real: the caller side marshalled `std::function` +
`std::promise`/`std::future` closures ("javascript", inversion of control).
Instead, **rewrite forward** from the CURRENT codebase into a clean
typed-command actor model — keeping the good parts `1e64178` produced and the
useful structure already present.

### What the current code gives us (keep / reuse)

- The engine already has a **typed, transport-free `ctl_*` API**
  (runtime.cpp:191-410): `ctl_start_child`, `ctl_delete_child`,
  `ctl_restart_child`, `ctl_suspend_child`, `ctl_resume_child`,
  `ctl_terminate_child`, `ctl_on_heartbeat`, `ctl_on_send_timeout`,
  `ctl_configure_trace`, `ctl_configure_log_level`, `ctl_get_tree`,
  `ctl_get_system_info`, `ctl_get_trace_config`. Each wraps a `do_*` primitive
  (runtime.cpp:1394+). This is a CLEANER engine boundary than the old closures —
  we keep it, just move it behind the queue and drop the lock.
- The **command queue ingress already exists**: `post_command`/`drain_commands`
  (runtime.cpp:750/768), `cmd_mutex_` guards ONLY the queue, `cmd_eventfd_`
  wakes the loop, drain runs on the loop thread before reap/sample/emit.
- The **resolve+cast lift** (SupervisorCtl_handlers.cc:149-182) — correct, stays.
- Engine is transport- and protobuf-free; SupervisorCtl owns all casting.

## Decision

Rewrite the executor surface into a **typed-command actor model**:

- Callers (SupervisorCtl handlers) build a **typed `ExecCommand`** and either
  `enqueue` (cast) or `call` (reply). No `std::function`, no `state_mu_`.
- The loop thread is the **sole owner** of fork/exec/waitpid/SIGCHLD/child table.
  The `ctl_*` methods become the loop-thread implementations the queue dispatches
  to; their bodies lose the `state_mu_` lock entirely.
- `cmd_mutex_` (queue ingress) is the ONLY remaining control-path lock; it is
  never exposed to peer threads.
- `casts` (heartbeat, send-timeout) → fire-and-forget `enqueue`.
- `CALLs` needing a gen_server reply (get_tree, get_system_info,
  get_trace_config, start/delete/restart/suspend status, configure-* status)
  → `call` + wait on a **single per-call promise** (typed reply, not a
  `std::function` round-trip).

## Design

### 1. Typed command surface over the existing queue

Keep `post_command`'s queue/eventfd/drain mechanism but stop pushing
`std::function`. Add a typed `ExecCommand` the engine switches on, dispatching
to the existing `ctl_*` methods (now lock-free, loop-thread-only).

```cpp
// engine-side (runtime.h)
struct ExecCommand {
    enum class Op { StartChild, DeleteChild, RestartChild, SuspendChild,
                    ResumeChild, TerminateChild, OnHeartbeat, OnSendTimeout,
                    ConfigureTrace, ConfigureLogLevel,
                    GetTree, GetSystemInfo, GetTraceConfig, Shutdown };
    Op op;
    // typed args (small POD/string fields per op; a flat struct, NOT a union —
    // keeps it trivially copyable + readable, matches the ctl_* signatures).
    ...
    // reply channel — present ONLY for CALL-shaped ops:
    std::promise<ExecReply>* reply = nullptr;   // null => fire-and-forget cast
};
```

The engine exposes two thin entry points (the ONLY surface peer threads see):

```cpp
void   Supervisor::enqueue(ExecCommand cmd);            // cast: returns now
ExecReply Supervisor::call(ExecCommand cmd);            // CALL: blocks on reply
```

Both push onto the SAME `cmd_queue_` (cmd_mutex_ + eventfd). `call()` attaches a
local `std::promise`, enqueues, `fut.get()`s — exactly one promise per CALL, no
`std::function`. `drain_commands()` runs each command on the loop thread via a
single `switch (cmd.op)` that calls the existing `ctl_*` methods (→ `do_*`
primitives) and, for CALL ops, sets the promise.

Decision to settle in implementation: whether `cmd_queue_` becomes
`std::deque<ExecCommand>` (drop the `std::function` queue) or `ExecCommand`
carries the dispatch inline. Prefer the typed deque — `drain_commands` owns the
`switch`, keeping all dispatch in one place on the loop thread.

### 2. SupervisorCtl handlers become enqueue/call sites

Each handler builds a typed `ExecCommand` and either `enqueue`s (cast) or
`call`s (reply). No `eng->ctl_*()` direct calls, no `state_mu_`. Examples:

```cpp
void handle_cast(const HeartbeatReport& m, ...) {
    if (auto* e = engine())
        e->enqueue({Op::OnHeartbeat, s(m.node_name), (pid_t)m.pid, m.seq});
}
ControlReply handle_call(const StartChildRequest& req, ...) {
    auto* e = engine();
    if (!e) { ControlReply r; set_reply(r, 4, s(req.spec.name)); return r; }
    auto rep = e->call({Op::StartChild, /*parent,name,cmd,...*/});
    ControlReply r; set_reply(r, rep.status, s(req.spec.name)); return r;
}
```

### 3. Reads: snapshot on the loop, returned by value

`GetTree` / `GetSystemInfo` / `GetTraceConfig` are CALL ops too — they run on
the loop thread (sole owner reads its own tree, no lock) and return the
`std::vector<TreeRow>` / `SystemInfoData` / `vector<TraceConfigRow>` through the
promise. Same data, no `state_mu_`.

### 4. resolve+cast stays exactly as-is (do NOT move into the queue)

The trace/log path is already correct and must NOT be queued:
- `resolve_node()` is a **pure read** (`resolve_trace_target`, const tree
  lookup) — fine to call from the ctl thread.
- The **cast** must happen on `g_ctl` (SupervisorCtl, runtime-backed); the
  engine loop is a bare runnable that CANNOT touch TIPC. This is the standing
  "runnable can't cast" constraint.

So `ConfigureTrace`/`ConfigureLogLevel` split:
- **store** (mutates `trace_configs_`/`log_levels_`) → CALL op on the loop
  thread (`Op::ConfigureTrace` returns the resolve-validated status).
- **live push** (resolve + cast `cfg.trace_ctrl`/`cfg.log_level`) → stays on the
  ctl thread via the existing `resolve_and_cast<Msg>` / `ctl_set_*`
  (SupervisorCtl_handlers.cc:149-182). UNCHANGED.
- The engine's restart re-push EmitForwarder hop (`set_trace`/`set_log_level` →
  `ctl_set_*`) also UNCHANGED — it already defers the cast to the ctl thread.

NOTE the store-then-push ordering: today the handler calls `ctl_configure_*`
(store) then `ctl_set_*` (cast). With the store now behind a CALL, the handler
does `auto ok = e->call({Op::ConfigureTrace,...}); if (ok.status==0)
ctl_set_trace(target, cfg.trace_ctrl);` — store completes on the loop before the
ctl-thread cast, same order as today.

### 5. Delete `state_mu_` and the recursive workaround

Remove `state_mu_` (runtime.h), the recursive_mutex, every
`lock_guard<recursive_mutex>` added in `1e64178` (the ctl_* methods + the
per-tick `tick_lk` in `run()`). The `ctl_*` methods keep their bodies (they wrap
`do_*`) but lose the lock — they now run ONLY on the loop thread via the
`drain_commands` switch, so no lock is needed. `cmd_mutex_` (queue ingress) is
the sole remaining lock on the control path.

## Relationship to the signal-ownership fix

Orthogonal and complementary — do both. The signal fix (engine blocks SIGCHLD
only; SIGTERM/SIGINT to generic main.cc — see
[supervisor-signal-ownership.md](supervisor-signal-ownership.md)) keeps SIGCHLD
on the loop's signalfd, which is exactly the actor model's exec owner. Together:
one thread owns SIGCHLD + fork + waitpid + child table + the command queue
drain; peer threads only enqueue.

## Verification

1. Build `//platform/supervisor/main:supervisor`.
2. Standalone + tdb over TIPC (same harness as the 1e64178 live test):
   - `ps` / `supervisor` / `trace-config` reads return correctly (CALL path).
   - `restart` ×N incl. an 8× rapid stress: child pid cycles, status=0, no hang.
   - terminate-hold → stopped, then restart → running.
   - trace on/off → child Tracer flips (resolve+cast path intact).
   - log level set → child level changes.
3. Concurrency assertion (the point of the revert): hammer `restart` from tdb
   in a tight loop WHILE a child crashes repeatedly; confirm SIGCHLD reaping is
   never starved — restart latency stays bounded (no reap-behind-control-lock
   stall). Contrast with `1e64178` where a control caller could hold the lock
   reap needs.
4. Grep clean: no `state_mu_`, no `recursive_mutex`, no `run_on_engine` in the
   supervisor tree.

## Status

`1e64178` is already pushed to `psp-retirement`. This is a NEW forward commit
(no history rewrite, no revert) that rewrites the executor concurrency model from
the current codebase, KEEPING the genuinely-good parts of `1e64178` (engine is
transport- and protobuf-free; SupervisorCtl owns all casting; the resolve+cast
lift; the typed `ctl_*` engine API) and dropping only the direct-call +
`state_mu_` coupling in favour of the typed-command queue.

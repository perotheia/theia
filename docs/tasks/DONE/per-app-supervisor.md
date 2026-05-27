[tag:blocked-by:Erlang-style-supervisor-spec]


# Per-app supervisor (in-process actor supervision)

The OS-level supervisor (`docs/supervision.md`) manages daemon
*processes*. The work tracked here is the layer below: an **in-process
supervisor** that manages a group of actor nodes inside one daemon —
the equivalent of an Erlang application's top supervisor tree, not
the OS-supervised init.

Today a single application/service in our system cannot supervise its
own nodes. If one node crashes (uncaught exception inside a handler,
state corruption, etc.), the rest keep running but nothing restarts
the dead one. We'd like to declare per-app supervision in the `.art`,
have the generator emit the supervision tree, and have the runtime
restart dead nodes within the daemon according to the declared
strategy.

## Open work

1. extend the `.art` grammar with `supervisor` declarations alongside
   `node atomic`. Each supervisor names its children (other nodes
   and other supervisors) and a restart strategy
   (`one_for_one` / `one_for_all` / `rest_for_one`).
2. extend `platform/runtime/` with a `Supervisor` class that observes
   its children's threads, classifies crashes (uncaught exception,
   `terminate` returning non-normal, watchdog timeout), and applies
   the strategy.
3. extend `artheia gen-app-composition` to emit supervisor wiring at
   the top of each generated `main.cc`: instead of constructing
   nodes directly, construct the supervisor tree and let it construct
   children.
4. wire a tracer hook: on a supervisor-detected restart, automatically
   enable `tracer_for(child_name).enable(true)` for the next N restarts,
   so the crash window is captured in the trace stream.

## Relation to neighbors

- `docs/supervision.md` — OS-level process supervisor; one level above.
- `docs/runtime.md` — actor model that the in-process supervisor
  manages.
- The OS supervisor's per-process tombstones (`docs/tombstone.md`)
  cover signal-induced crashes; the in-process supervisor here covers
  exceptions inside handlers, which don't trigger a signal.
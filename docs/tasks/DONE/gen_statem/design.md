# gen_statem MVP — design

Companion to `README.md` (the original ticket). This file lays out
**what we'll build** before we write any code: the C++ `GenStateM<T,S,D>`
API surface, the `.art` `statem { ... }` grammar, the lowering between
them, the SM walkthrough, and the test plan.

Scope confirmed:

- **Option A + C** — minimal C++ class + artheia grammar extension
  + codegen.
- **First consumers**: self-contained tests in `platform/runtime/test/`,
  then `sm` (the platform State Management FC) migrates to use it.

## Influences

- **Erlang `gen_statem`** (`up/otp/lib/stdlib/src/gen_statem.erl`,
  5413 lines) — provides the *behavioural contract*: callback module
  exposes `init/1`, `handle_event/4`, `terminate/3`; framework runs
  the state-mailbox loop, schedules state timeouts, postpones events,
  reads back state-enter calls. We adopt the **`handle_event_function`
  callback mode** (single dispatch per event, state is arbitrary
  term) — NOT the legacy `state_functions` mode (one function per
  state). One handler keeps the C++ ergonomic.
- **hsmcpp** (`up/hsmcpp/`) — provides the *static declaration*
  ergonomics: states / events / transitions / sub-states / actions
  declared once, generator emits a base class, the user fills in
  callbacks. We adopt the **"declare once, generate skeleton"
  pattern** but NOT the SCXML format (artheia is our DSL).
- **Our `GenServer`** (`platform/runtime/include/GenServer.hh`) —
  the actor runtime we extend. `GenStateM` is a layer ON TOP, not
  a parallel runtime: one thread, one mailbox, the same trace points.

## Non-goals (explicit non-features for v1)

- **Multiple callback modes.** We always use `handle_event` style.
- **Dynamic callback-module swap.** No `change_callback_module/1` —
  the FSM type is fixed at compile time (this is C++, not Erlang).
- **Postpone with arbitrary depth.** Postpone is supported (single
  re-queue on state change), but the spec's `keep_state_and_data`
  with deep retry semantics is out.
- **Self-generated events with priority ordering.** A state can
  request a follow-up event; it goes to the back of the mailbox,
  not the front. Erlang's `next_event` with reorder semantics is
  out.
- **State-enter loops.** Erlang allows the `state_enter` callback
  to itself request a transition; we forbid this in v1 (state
  enter is action-only — emit messages, set timers, log).
- **SCXML round-trip.** hsmcpp's value-add is the SCXML editor;
  our value-add is artheia. The two don't need to interop — if
  someone wants SCXML, write a separate `artheia import-scxml`
  one-shot later.
- **History pseudostates.** Resume-where-left-off requires
  persistent state per parent. Useful but not v1.

## C++ surface: `GenStateM<Derived, StateT, DataT>`

Lives at `platform/runtime/include/GenStateM.hh`. Derives from
`GenServer<Derived, StateMHolder<StateT, DataT>>` so the existing
mailbox / dispatcher / tracer flow is reused.

### The shape

```cpp
namespace platform::runtime {

// Result of handle_event. Three constructors mirror Erlang's
// return-value vocabulary, simplified.
template <typename S>
struct EventResult {
    enum Kind { kKeep, kKeepData, kTransition, kPostpone, kHalt };
    Kind kind{kKeep};
    S new_state{};                       // valid when kind == kTransition
    std::optional<int64_t> state_timeout_ms;   // optional timeout set at this event
};

template <typename S>
EventResult<S> keep_state();
template <typename S>
EventResult<S> transition_to(S new_state);
template <typename S>
EventResult<S> postpone();               // re-queue on next state change
template <typename S>
EventResult<S> halt();                   // stop the state machine

// Combinators (optional timeout after a transition):
template <typename S>
EventResult<S> transition_to(S new_state, int64_t timeout_ms);

}  // namespace platform::runtime
```

### The derived shape

```cpp
class SmDaemon
    : public platform::runtime::GenStateM<SmDaemon, SmState, SmData> {
public:
    static constexpr const char* kNodeName = "sm";

    SmState init(SmData& d) {           // analogous to gen_server's init
        return SmState::Off;
    }

    // One handler per (StateT, EventT) pair. Overload resolution +
    // template deduction picks the right one. Unknown (state, event)
    // pairs fall through to the framework's "drop with warning"
    // default — Erlang's hot-loop has this same property.
    EventResult<SmState> handle_event(
        SmState s, const SystemBoot& e, SmData& d) {
        if (s == SmState::Off) return transition_to(SmState::Starting,
                                                    /*timeout_ms=*/30'000);
        return keep_state();
    }

    EventResult<SmState> handle_event(
        SmState s, const StartupComplete& e, SmData& d) {
        if (s == SmState::Starting) return transition_to(SmState::Running);
        return keep_state();
    }

    // State-timeout event — the framework synthesises this when the
    // timeout set via `transition_to(..., timeout_ms)` fires.
    EventResult<SmState> handle_event(
        SmState s, const StateTimeout& e, SmData& d) {
        if (s == SmState::Starting) return transition_to(SmState::Degraded);
        return keep_state();
    }

    // Optional state-enter hook (Erlang's enter_loop). Called AFTER
    // every transition with the new + old state. Action-only: must
    // not call transition_to from here (compile-time check via
    // returning void).
    void on_enter(SmState new_s, SmState old_s, SmData& d) {
        log_info("sm: state " + name(old_s) + " → " + name(new_s));
        broadcast_state_change_(new_s);   // sender-port emission
    }
};
```

### How it composes with GenServer

`GenStateM` IS a `GenServer<Derived, StateMHolder>` where
`StateMHolder` is the internal `{ S state; D data; }` pair. The
mailbox dispatcher receives `cast`/`call`/`info`; the dispatcher
funnels typed message arrivals into `handle_event(state, msg, data)`
via a template indirection. Trace points fire as usual (the
existing `Send`/`Recv`/`Dispatch`/`DispatchDone` events get a new
peer event: `StateTransition` carrying old_state, new_state, event).

### State timeouts

A `transition_to(NewState, timeout_ms)` is a hint to the framework:
on entering NewState, set a `Timer` for `timeout_ms` that posts a
`StateTimeout{from: NewState}` message back to the mailbox. The
timer is **scoped to that state** — any subsequent transition out
of NewState cancels it. We piggyback on the existing `TimerService`
(`platform/runtime/include/TimerService.hh`) so this is zero new
machinery.

### Postpone

`postpone()` causes the framework to re-enqueue the event at the
back of the mailbox AND mark it sticky-until-state-change. On the
next transition the postponed event is re-delivered as the first
event in the new state. Single-depth — postponing a postponed
event drops it (the spec's deeper semantics are too easy to abuse).

### Tracing

`StateTransition` is a new `TraceEvent` enum value. Carries
`old_state`, `new_state`, `event_name`, `correlation_id`. The
existing `tracer_for(kNodeName)` flow handles it — supervisor-gui's
Trace panel picks it up automatically. Wins: post-mortem inspection
shows the FSM history side-by-side with the cast/call traffic that
caused it.

## artheia `.art` grammar: `statem { ... }`

Grammar extension to `node atomic`:

```
node atomic SmDaemon {
    tipc type=0x8001000D instance=0
    ports {
        sender broadcast provides SmStateStream
        server ctl       provides StateMgmtCtl
    }

    statem {
        states  [Off, Starting, Running, Degraded, Update, Shutdown]
        initial Off
        data    SmData         // typed data carried through the FSM

        on Off:
            event SystemBoot       → Starting after 30s

        on Starting:
            event StartupComplete  → Running
            timeout                → Degraded

        on Running:
            event ShutdownRequest  → Shutdown
            event UpdateRequest    → Update

        on Update:
            event UpdateComplete   → Running

        on Degraded:
            event RetryStartup     → Starting after 30s

        on Shutdown:
            event PowerOff         → halt
    }
}
```

### Grammar additions (textX)

```
NodeDecl:
    ... (existing fields) ...
    ('statem' '{' statem=StateMBody '}')?
;

StateMBody:
    'states'  '[' states*=ID[','] ']'
    'initial' initial=ID
    ('data' data_type=[MessageDecl|FQN])?
    on_blocks*=StateBlock
;

StateBlock:
    'on' state=ID ':'
        rules*=TransitionRule
;

TransitionRule:
    ('event'   event=[MessageDecl|FQN] '→' target=TransitionTarget) |
    ('timeout' '→' target=TransitionTarget)
;

TransitionTarget:
    halt?='halt' | (state=ID ('after' timeout=Duration)?)
;

Duration:
    /[0-9]+(ms|s|m|h)/    // e.g. "30s", "500ms"
;

// Note: `states` / `initial` / `state` use bare `ID` rather than
// `[ID]` because textX treats `[ID]` as a cross-ref to instances of
// class `ID`, but `ID` is a primitive (built-in token). The bare-ID
// form captures the token as a string. Validation of "initial is a
// declared state" / "transition target is a declared state" lives in
// Python (StateMSpec.validate), not textX.
```

### Lowering: `.art` → C++

`gen-cpp-stubs` learns to emit `<NodeName>StateMBase.hpp` when a node
has a `statem` block:

```cpp
// AUTO-GENERATED by `artheia gen-cpp-stubs` — DO NOT EDIT.
// Source: services/system/sm/package.art

enum class SmState { Off, Starting, Running, Degraded, Update, Shutdown };

class SmDaemonStateMBase
    : public platform::runtime::GenStateM<SmDaemon, SmState, SmData> {
protected:
    SmState init(SmData& d) override { return SmState::Off; }

    // Generated dispatch shells — derived class fills in the actual
    // transitions via overrides of the per-(state, event) hooks.
    EventResult<SmState> handle_event(
        SmState s, const SystemBoot& e, SmData& d) {
        if (s == SmState::Off)
            return transition_to(SmState::Starting, /*timeout_ms=*/30'000);
        return keep_state();
    }
    EventResult<SmState> handle_event(
        SmState s, const StartupComplete& e, SmData& d) {
        if (s == SmState::Starting) return transition_to(SmState::Running);
        return keep_state();
    }
    // ... one method per declared event ...

    EventResult<SmState> handle_event(
        SmState s, const platform::runtime::StateTimeout& e, SmData& d) {
        if (s == SmState::Starting) return transition_to(SmState::Degraded);
        if (s == SmState::Degraded) return transition_to(SmState::Starting,
                                                          /*timeout_ms=*/30'000);
        return keep_state();
    }
};
```

The DERIVED class (`SmDaemon`) is allowed to override `on_enter` for
side effects (broadcast, log) and to override any specific
`handle_event` overload to add conditions beyond the static
transition table. The base provides the structural skeleton; the
derived adds the application logic.

## SM walkthrough (concrete first consumer)

`services/system/sm/package.art` already declares:

```
enum SmState {
    OFF = 0, STARTING = 1, RUNNING = 2,
    DEGRADED = 3, UPDATE = 4, SHUTDOWN = 5
}
```

After this work, the same file gains:

```
node atomic SmDaemon {
    tipc type=0x8001000D instance=0
    ports {
        sender   broadcast provides SmStateStream
        server   ctl       provides StateMgmtCtl
        client   to_per    requires PersistencyIf
        sender   to_log    provides LogStream  best_effort
    }

    statem {
        states  [OFF, STARTING, RUNNING, DEGRADED, UPDATE, SHUTDOWN]
        initial OFF
        data    SmStateMsg

        on OFF:
            event SmRequest        → STARTING after 30s

        on STARTING:
            event SystemReady      → RUNNING
            timeout                → DEGRADED

        on RUNNING:
            event SmRequest        → UPDATE        // (only if r.target==UPDATE)
            event ShutdownRequest  → SHUTDOWN

        on DEGRADED:
            event SmRequest        → STARTING after 30s

        on UPDATE:
            event UpdateComplete   → RUNNING

        on SHUTDOWN:
            event PowerOff         → halt
    }
}
```

The C++ side: `SmDaemon` inherits the generated `SmDaemonStateMBase`,
overrides `on_enter` to send the broadcast on `SmStateStream` and
persist to `per` via the client port, overrides `handle_event` for
`SmRequest` to narrow the dispatch (a `SmRequest` in RUNNING goes
to UPDATE iff its `target == UPDATE`, not unconditionally).

## Test plan

`platform/runtime/test/test_gen_statem.cpp` — three tests, each ~80 LOC:

1. **traffic_light** — 3 states (RED / GREEN / YELLOW) cycling on
   state timeouts. Asserts:
   - sequence of state transitions over 3 timeout cycles
   - tracer captures `StateTransition` events with correct
     old/new pair
   - state timeout fires once per state (no leakage across
     transitions)

2. **retry_with_escalate** — 2 states (TRYING / FAILED).
   `RetryEvent` keeps re-trying; after 3 retries within 5 s,
   escalate to FAILED. Asserts:
   - retry counter in `data` increments
   - escalation transition happens at the right count
   - postpone() works: a deferred event arrives after escalation
     completes

3. **door_lock** — 3 states + guard conditions. Hand-written
   handle_event override returns `keep_state()` or
   `transition_to()` based on `data.has_keycard`. Asserts:
   - guards correctly gate transitions
   - generated dispatch routes to derived override

## Implementation phases

1. **Phase 1 (this design doc)** — DONE this turn. Lock the API.
2. **Phase 2** — C++ `GenStateM.hh` + tests. ~1-2 days. Bazel
   target `//platform/runtime:gen_statem`. Three tests in
   `platform/runtime/test/`.
3. **Phase 3** — Grammar (`artheia.tx`) + Python AST plumbing
   (`artheia/manifest/statem.py` analogous to `cluster.py`) + LSP
   keywords. Parse-only; no codegen yet. ~1 day.
4. **Phase 4** — `gen-cpp-stubs` lowering: emit `<Name>StateMBase.hpp`
   from a node's statem block. ~1 day.
5. **Phase 5** — SM migration: declare the SM statem block in
   `services/system/sm/package.art`, write `SmDaemon` deriving
   from the generated base. ~1 day.
6. **Phase 6 (deferred)** — supdbg gains a `statem` subcommand:
   live current-state introspection over services/com gRPC.

Total ~5-6 days of focused work; checkpoints between phases.

## Decisions ratified (2026-05-23)

1. **`data` field**: declared as a `.art` `MessageDecl`, codegen'd
   to a POD, used directly by the derived class. No special
   serialization path on transitions — the field is in-process
   state, not wire data. Uniform with every other message in
   artheia.
2. **`halt` semantics**: two methods, exit-code based.
   - `halt()` → process exits with code 0 (clean stop).
   - `halt_with_error("reason")` → process exits with code 1+
     (faulted stop).
   Supervisor's existing `on_child_exit`
   (`platform/supervisor/src/runtime.cpp`) inspects exit codes
   against `RestartType` — Permanent restarts on either code,
   Transient restarts only on non-zero, Temporary never restarts.
   No new supervisor logic needed.
3. **`on_enter` callback access**:
   - Returns `void` (compile-time forbids transitions —
     `EventResult<S>` is producible only from `handle_event`).
   - May call `cast()`, `call()`, `post_info()`,
     `TimerService::send_after()`. Message sends and timer arms
     are explicitly allowed — required for SM's broadcast-on-enter.
   - "Auto-advance" states (no event, immediate next state) use
     `transition_to(NewState, /*timeout_ms=*/0)` from the
     *incoming event's* handler, not from `on_enter`. This keeps
     traces honest (every transition is a real, observable event)
     and the state graph statically determined by the `.art`.

## Out of scope (deferred, post-MVP)

- supdbg `statem` subcommand (Phase 6).
- Visualisation in supervisor-gui (FSM tab showing live state).
- SCXML import (`artheia import-scxml <file>` → emit statem block).
- Cross-FSM event correlation (one FSM's transition triggers
  another's event). Today: route via cast() like any other inter-
  node message.
- Persistent state (FSM resume after restart). Today: cold start
  always returns to `initial`. Persistent state needs services/db.

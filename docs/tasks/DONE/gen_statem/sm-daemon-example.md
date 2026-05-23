# SmDaemon — consumer skeleton

This is the C++ shape the eventual `services/system/sm/` binary should
take, once `sm` actually has code. The generated base
(`SmDaemonStateMBase.hh`) handles the static transition table; the
hand-written `SmDaemon` adds the side effects.

This file is **not built** — it's reference material for the engineer
who picks up `sm`. The generated base is the contract; this example
shows how to fulfil it.

## Build inputs

- `services/system/sm/package.art` — has the `statem { ... }` block.
- Codegen (`artheia gen-cpp-stubs services/system/sm/package.art --out <gen-dir>`)
  produces:
  - `SmDaemon_gen.h` — callback-style stub (existing pattern)
  - `SmDaemonStateMBase.hh` — gen_statem base class

The eventual Bazel rule wires both into a `cc_library`, plus the
hand-written `sm_daemon.cc` below.

## Example `sm_daemon.cc`

```cpp
// services/system/sm/src/sm_daemon.cc
#include "SmDaemonStateMBase.hh"
#include "Logger.hh"
#include "NodeRef.hh"
#include "RuntimeContext.hh"

namespace system_services_sm {

class SmDaemon : public SmDaemonStateMBase {
public:
    static constexpr const char* kNodeName = "sm";

    explicit SmDaemon(demo::runtime::NodeRef<...> per_client,
                       demo::runtime::SenderRef<SmStateStream> broadcast)
        : per_(per_client), broadcast_(broadcast) {}

    // ---- on_enter: the side-effect hook --------------------------------
    //
    // Runs AFTER every committed transition. Cast() / call() are fine;
    // transition_to() is NOT — that's why on_enter returns void. The
    // base provides the transition table; this just reacts to it.
    void on_enter(SmDaemonState new_s, SmDaemonState /*old_s*/,
                   SmStateMsg& d) {
        // 1. Update the data carried through the FSM.
        d.state = static_cast<SmState>(new_s);
        d.ts_ns = now_ns();

        // 2. Broadcast to all subscribers via the sender port.
        demo::runtime::cast(broadcast_, d);

        // 3. Persist via per (best-effort — drop if per is wedged).
        demo::runtime::cast(per_, /*write op carrying d*/);

        // 4. Log the transition for postmortem.
        log_info_("sm: → " + std::string(name_of(new_s)));
    }

    // ---- handle_event overrides: add guards ---------------------------
    //
    // The base's handle_event(SmDaemonState, const UpdateRequest&, ...)
    // unconditionally moves RUNNING→UPDATE. We override here to gate
    // on an "update is allowed right now" check — a real SM has policy
    // beyond "this state has an outgoing edge for this event".
    using SmDaemonStateMBase::handle_event;
    demo::runtime::EventResult<SmDaemonState> handle_event(
            SmDaemonState s, const UpdateRequest& e, SmStateMsg& d) {
        if (s == SmDaemonState::RUNNING && update_allowed_()) {
            return demo::runtime::transition_to<SmDaemonState>(
                SmDaemonState::UPDATE);
        }
        return demo::runtime::keep_state<SmDaemonState>();
    }

    // ---- StateMgmtCtl::RequestMode — server-port handler --------------
    //
    // Maps the external SmRequest{target} message into one of the
    // FSM-internal events. Generated _gen.h declares this signature.
    SmEmpty handle_call(const SmRequest& req, GenStateMHolder<...>& /*h*/) {
        switch (req.target) {
        case SmState::SHUTDOWN:
            demo::runtime::post_event(*this, ShutdownRequest{});
            break;
        case SmState::UPDATE:
            demo::runtime::post_event(*this, UpdateRequest{});
            break;
        default:
            /* unknown target — reject silently, or extend SmEmpty
               to carry a status code if you need feedback */
            break;
        }
        return SmEmpty{};
    }

private:
    demo::runtime::NodeRef<...> per_;
    demo::runtime::SenderRef<SmStateStream> broadcast_;
    bool update_allowed_() { /* policy check */ return true; }
};

}  // namespace system_services_sm

// ---- entry point ---------------------------------------------------------

int main() {
    demo::runtime::RuntimeContext ctx;
    /* construct per client + broadcast sender from ctx, then: */
    system_services_sm::SmDaemon sm(per, broadcast);
    sm.start_statem(ctx.timers());

    /* Driver: post_event() the external events that arrive from
       other FCs (exec posts SystemBoot, phm posts StartupComplete /
       RetryStartup, ucm posts UpdateComplete, ...). */

    sm.stop();
    return 0;
}
```

## What the generator gives you

For the `.art` block:

```
statem {
    states  [OFF, STARTING, RUNNING, DEGRADED, UPDATE, SHUTDOWN]
    initial OFF
    data    SmStateMsg
    on OFF: event SystemBoot → STARTING after 30s
    on STARTING:
        event StartupComplete → RUNNING
        timeout               → DEGRADED
    on RUNNING:
        event ShutdownRequest → SHUTDOWN
        event UpdateRequest   → UPDATE
    on UPDATE:    event UpdateComplete → RUNNING
    on DEGRADED:  event RetryStartup   → STARTING after 30s
    on SHUTDOWN:  event PowerOff       → halt
}
```

…the codegen emits:

- `enum class SmDaemonState : uint8_t { OFF=0, STARTING=1, ... SHUTDOWN=5 }`
- 7 `handle_event(SmDaemonState, const <Evt>&, SmStateMsg&)` overloads — one per declared event
- 1 `handle_event(SmDaemonState, const StateTimeoutMsg<SmDaemonState>&, SmStateMsg&)` — switches on state for `STARTING → DEGRADED`
- `init()` returning `SmDaemonState::OFF`
- `class SmDaemon;` forward decl so `GenStateM<SmDaemon, ...>` instantiates

## Why on_enter is the right hook for broadcast

Erlang's `state_enter` callback (which our `on_enter` mirrors) is
guaranteed to run after every committed transition AND on FSM init.
Putting the broadcast there gives every subscriber a starting state
on subscription, AND notifications on every change — exactly the
ASAM-CMP / OEM "current platform state" contract.

If you broadcast from inside `handle_event` instead, you'd need to
broadcast on every overload AND duplicate the broadcast in `init`.
Putting it in `on_enter` is one place.

## What to do when sm actually gets built

1. Write a `services/system/sm/BUILD.bazel` `cc_binary` that:
   - depends on `//platform/runtime:runtime`
   - codegens `SmDaemonStateMBase.hh` and `SmDaemon_gen.h` via a
     `genrule` calling `artheia gen-cpp-stubs`
   - codegens the pb.h files for the messages
   - compiles `src/sm_daemon.cc` plus the codegen outputs
2. Wire it into `services/manifest/service.py` (it's already in the
   supervisor tree under `core_sup`; just add an Executable).
3. Smoke test: drive `SystemBoot → StartupComplete → ShutdownRequest →
   PowerOff` and assert the broadcast stream emits the expected
   sequence.

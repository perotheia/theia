// demo::runtime::GenStateM<Derived, StateT, DataT>
//
// C++17 take on Erlang's gen_statem (handle_event_function callback
// mode). Modeled on up/otp/lib/stdlib/src/gen_statem.erl, layered on
// our existing GenServer<Derived, State> so the per-node thread,
// mailbox, tracer, and TimerService machinery are reused — no parallel
// runtime.
//
// User-facing shape (see docs/tasks/PROGRESS/gen_statem/design.md):
//
//   class SmDaemon
//     : public GenStateM<SmDaemon, SmState, SmData> {
//   public:
//       static constexpr const char* kNodeName = "sm";
//
//       SmState init(SmData& d) { return SmState::Off; }
//
//       // One overload per (state-set, event-type). Overload resolution
//       // picks. Unknown (state, event) drops with a trace warning —
//       // mirrors Erlang's "no clause matches → keep_state_and_data".
//       EventResult<SmState> handle_event(
//           SmState s, const SystemBoot& e, SmData& d) {
//           if (s == SmState::Off)
//               return transition_to(SmState::Starting, 30'000);
//           return keep_state<SmState>();
//       }
//
//       // Optional state-enter hook. Returns void → compile-time
//       // forbid of transition_to from here. May call cast() / send_after.
//       void on_enter(SmState new_s, SmState old_s, SmData& d) { ... }
//   };
//
//   GenStateM drives via free function post_event(server, msg).
//
// Semantics:
//   keep_state                      no transition, no timeout change
//   keep_state_and_reset_timeout    keep state, re-arm state timeout
//   transition_to(NewState)         commit transition, cancel timeout
//   transition_to(NewState, ms)     commit + arm new state-timeout
//   postpone                        re-queue event after next transition
//   halt(reason)                    clean exit (code 0)
//   halt_with_error(reason)         faulted exit (code 1)
//
// Non-goals (see design.md): multiple callback modes, dynamic callback
// module swap, priority-ordered next_event, on_enter transitions,
// history pseudostates.

#pragma once

#include "GenServer.hh"
#include "TimerService.hh"
#include "Tracer.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace demo {
namespace runtime {

// ---- EventResult -------------------------------------------------------

template <typename StateT>
struct EventResult {
    enum class Kind : uint8_t {
        Keep,                 // no transition, no timeout change
        KeepResetTimeout,     // keep state, re-arm state timeout
        Transition,           // move to new_state (with optional timeout)
        Postpone,             // re-queue this event after next transition
        Halt,                 // exit(0)
        HaltWithError,        // exit(1) + record reason
    };

    Kind  kind{Kind::Keep};
    StateT new_state{};
    std::optional<int64_t> state_timeout_ms;
    std::string halt_reason;  // populated for Halt*
};

template <typename StateT>
EventResult<StateT> keep_state() {
    return EventResult<StateT>{};
}

template <typename StateT>
EventResult<StateT> keep_state_and_reset_timeout(int64_t timeout_ms) {
    EventResult<StateT> r;
    r.kind = EventResult<StateT>::Kind::KeepResetTimeout;
    r.state_timeout_ms = timeout_ms;
    return r;
}

template <typename StateT>
EventResult<StateT> transition_to(StateT new_state) {
    EventResult<StateT> r;
    r.kind = EventResult<StateT>::Kind::Transition;
    r.new_state = std::move(new_state);
    return r;
}

template <typename StateT>
EventResult<StateT> transition_to(StateT new_state, int64_t timeout_ms) {
    EventResult<StateT> r;
    r.kind = EventResult<StateT>::Kind::Transition;
    r.new_state = std::move(new_state);
    r.state_timeout_ms = timeout_ms;
    return r;
}

template <typename StateT>
EventResult<StateT> postpone() {
    EventResult<StateT> r;
    r.kind = EventResult<StateT>::Kind::Postpone;
    return r;
}

template <typename StateT>
EventResult<StateT> halt(std::string reason = "normal") {
    EventResult<StateT> r;
    r.kind = EventResult<StateT>::Kind::Halt;
    r.halt_reason = std::move(reason);
    return r;
}

template <typename StateT>
EventResult<StateT> halt_with_error(std::string reason) {
    EventResult<StateT> r;
    r.kind = EventResult<StateT>::Kind::HaltWithError;
    r.halt_reason = std::move(reason);
    return r;
}

// ---- StateTimeoutMsg ----------------------------------------------------

// Synthetic event the framework posts when a state-timeout fires. Carries
// the state that was current when the timer was armed; the dispatcher
// drops the message if the state has moved on (cookie mismatch).
template <typename StateT>
struct StateTimeoutMsg {
    StateT from_state;
    uint64_t cookie;
};

// ---- Holder for state-machine internals ---------------------------------

template <typename StateT, typename DataT>
struct GenStateMHolder {
    StateT  state{};
    DataT   data{};

    // Active state-timer + the cookie active when it was armed.
    // expected_cookie monotonically increases on every transition;
    // the StateTimeoutMsg dispatcher drops messages with a stale cookie.
    TimerRef state_timer;
    uint64_t expected_cookie{0};

    // Postpone queue. Replay lambdas are pushed at the BACK on
    // postpone() and re-enqueued on the next transition.
    std::deque<std::function<void(GenServerBase*)>> postponed;

    bool initialized{false};
};

// ---- GenStateM ----------------------------------------------------------

template <typename Derived, typename StateT, typename DataT>
class GenStateM
    : public GenServer<Derived, GenStateMHolder<StateT, DataT>> {
public:
    using Holder = GenStateMHolder<StateT, DataT>;
    using Base   = GenServer<Derived, Holder>;
    using State  = StateT;
    using Data   = DataT;

    // User-supplied: returns the initial state. Default returns a
    // value-initialized StateT — fine for enum classes whose first
    // enumerator is the "start" state. Derived may shadow with its
    // own init().
    StateT init(DataT& /*data*/) { return StateT{}; }

    // Default on_enter — no-op. Derived may shadow.
    void on_enter(StateT /*new_s*/, StateT /*old_s*/, DataT& /*d*/) {}

    // Default handle_event for the synthetic StateTimeoutMsg. If
    // derived doesn't override, a state-timeout fire just resets the
    // state (no transition). Derived can shadow with its own overload
    // — overload resolution prefers the more-specific Msg type.
    EventResult<StateT> handle_event(StateT /*s*/,
                                       const StateTimeoutMsg<StateT>&,
                                       DataT& /*d*/) {
        return keep_state<StateT>();
    }

    // Default handle_info — no-op. GenServer's dispatch_info_ unconditionally
    // calls Derived::handle_info, so the base must provide it. Derived
    // may shadow.
    void handle_info(const char* /*info*/, Holder& /*h*/) {}

    // Default terminate — no-op. Same shape as GenServer's default.
    void terminate(const char* /*reason*/, Holder& /*h*/) noexcept {}

    // The TimerService the framework uses for state timeouts. Derived
    // may also use it for any non-state timers.
    TimerService& timers() {
        // Programmer error to call timers() before start_statem().
        return *timer_svc_;
    }

    // Start the GenStateM. Pass the TimerService that the framework
    // should use. Must be called before any post_event().
    void start_statem(TimerService& timer_svc) {
        timer_svc_ = &timer_svc;
        Base::start();
        // Run init() + first on_enter on the server thread.
        this->enqueue([](GenServerBase* base) {
            auto* self = static_cast<Derived*>(base);
            auto& h    = self->state();   // Holder&
            h.state = self->init(h.data);
            h.initialized = true;
            auto& tr = ::demo::runtime::tracer_for(Derived::kNodeName);
            if (tr.enabled()) {
                tr.emit(::demo::runtime::TraceEvent::StateTransition,
                        "<init>", /*corr=*/0, nullptr, 0);
            }
            self->on_enter(h.state, h.state, h.data);
        });
    }

private:
    TimerService* timer_svc_{nullptr};
};

// ---- Internal: arm a one-shot state timer -------------------------------

template <typename Derived, typename StateT, typename DataT>
void arm_state_timeout_(GenStateM<Derived, StateT, DataT>& server,
                         StateT for_state, int64_t timeout_ms);

template <typename Derived, typename StateT, typename DataT>
void post_state_timeout_msg(GenStateM<Derived, StateT, DataT>& server,
                             StateT armed_for, uint64_t cookie);

// ---- post_event: typed-message entry point -----------------------------

template <typename Derived, typename StateT, typename DataT, typename Msg>
void post_event(GenStateM<Derived, StateT, DataT>& server, Msg msg) {
    auto& tr = ::demo::runtime::tracer_for(Derived::kNodeName);
    uint32_t corr = tr.enabled()
        ? ::demo::runtime::next_trace_corr_id() : 0;
    if (tr.enabled()) {
        uint8_t scratch[256];
        uint16_t n = ::demo::runtime::encode_for_trace(
            msg, scratch, static_cast<uint16_t>(sizeof(scratch)));
        tr.emit(::demo::runtime::TraceEvent::Send,
                ::demo::runtime::msg_type_name<Msg>(), corr, scratch, n);
    }
    server.enqueue([m = std::move(msg), corr](GenServerBase* base) mutable {
        auto* self = static_cast<Derived*>(base);
        auto& h    = self->state();
        auto& tr2  = ::demo::runtime::tracer_for(Derived::kNodeName);
        const char* mname = ::demo::runtime::msg_type_name<Msg>();

        if (tr2.enabled()) {
            uint8_t scratch[256];
            uint16_t n = ::demo::runtime::encode_for_trace(
                m, scratch, static_cast<uint16_t>(sizeof(scratch)));
            tr2.emit(::demo::runtime::TraceEvent::Recv,  mname, corr,
                     scratch, n);
            tr2.emit(::demo::runtime::TraceEvent::Dispatch, mname, corr,
                     nullptr, 0);
        }

        StateT before = h.state;
        EventResult<StateT> r = self->handle_event(before, m, h.data);

        using K = typename EventResult<StateT>::Kind;
        switch (r.kind) {
        case K::Keep:
            break;
        case K::KeepResetTimeout:
            if (r.state_timeout_ms.has_value()) {
                arm_state_timeout_(*self, before, *r.state_timeout_ms);
            }
            break;
        case K::Transition: {
            if (h.state_timer.valid()) {
                self->timers().cancel_timer(std::move(h.state_timer));
                h.state_timer = TimerRef{};
            }
            h.expected_cookie++;
            h.state = r.new_state;
            if (tr2.enabled()) {
                tr2.emit(::demo::runtime::TraceEvent::StateTransition,
                         mname, corr, nullptr, 0);
            }
            while (!h.postponed.empty()) {
                auto fn = std::move(h.postponed.front());
                h.postponed.pop_front();
                base->enqueue(std::move(fn));
            }
            self->on_enter(h.state, before, h.data);
            if (r.state_timeout_ms.has_value()) {
                arm_state_timeout_(*self, h.state, *r.state_timeout_ms);
            }
            break;
        }
        case K::Postpone: {
            auto replay = [mcap = std::move(m), corr_replay = corr](
                              GenServerBase* base2) mutable {
                auto* self2 = static_cast<Derived*>(base2);
                auto& h2    = self2->state();
                auto& tr3   = ::demo::runtime::tracer_for(
                    Derived::kNodeName);
                const char* mn = ::demo::runtime::msg_type_name<Msg>();
                if (tr3.enabled()) {
                    tr3.emit(::demo::runtime::TraceEvent::Dispatch,
                             mn, corr_replay, nullptr, 0);
                }
                StateT before2 = h2.state;
                EventResult<StateT> rr =
                    self2->handle_event(before2, mcap, h2.data);
                using KK = typename EventResult<StateT>::Kind;
                if (rr.kind == KK::Transition) {
                    if (h2.state_timer.valid()) {
                        self2->timers().cancel_timer(
                            std::move(h2.state_timer));
                        h2.state_timer = TimerRef{};
                    }
                    h2.expected_cookie++;
                    h2.state = rr.new_state;
                    if (tr3.enabled()) {
                        tr3.emit(::demo::runtime::TraceEvent::
                                     StateTransition,
                                 mn, corr_replay, nullptr, 0);
                    }
                    self2->on_enter(h2.state, before2, h2.data);
                    if (rr.state_timeout_ms.has_value()) {
                        arm_state_timeout_(*self2, h2.state,
                                            *rr.state_timeout_ms);
                    }
                }
                if (tr3.enabled()) {
                    tr3.emit(::demo::runtime::TraceEvent::DispatchDone,
                             mn, corr_replay, nullptr, 0);
                }
            };
            h.postponed.push_back(std::move(replay));
            break;
        }
        case K::Halt:
        case K::HaltWithError: {
            int code = (r.kind == K::HaltWithError) ? 1 : 0;
            if (tr2.enabled()) {
                tr2.emit(::demo::runtime::TraceEvent::Terminate,
                         r.halt_reason.c_str(), corr, nullptr, 0);
            }
            self->terminate(r.halt_reason.c_str(), h);
            std::fflush(stdout);
            std::fflush(stderr);
            std::_Exit(code);
        }
        }

        if (tr2.enabled()) {
            tr2.emit(::demo::runtime::TraceEvent::DispatchDone, mname,
                     corr, nullptr, 0);
        }
    });
}

// ---- arm / post_state_timeout_msg definitions --------------------------

template <typename Derived, typename StateT, typename DataT>
void arm_state_timeout_(GenStateM<Derived, StateT, DataT>& server,
                         StateT for_state, int64_t timeout_ms) {
    auto* self = static_cast<Derived*>(&server);
    auto& h    = self->state();
    uint64_t cookie = h.expected_cookie;
    if (timeout_ms <= 0) {
        post_state_timeout_msg(server, for_state, cookie);
        return;
    }
    h.state_timer = server.timers().send_after_lambda(
        static_cast<int>(timeout_ms),
        [&server, for_state, cookie]() {
            post_state_timeout_msg(server, for_state, cookie);
        });
}

template <typename Derived, typename StateT, typename DataT>
void post_state_timeout_msg(GenStateM<Derived, StateT, DataT>& server,
                             StateT armed_for, uint64_t cookie) {
    server.enqueue([armed_for, cookie](GenServerBase* base) mutable {
        auto* self = static_cast<Derived*>(base);
        auto& h    = self->state();
        if (cookie != h.expected_cookie) {
            return;   // stale timer — state moved on
        }
        auto& tr2 = ::demo::runtime::tracer_for(Derived::kNodeName);
        if (tr2.enabled()) {
            tr2.emit(::demo::runtime::TraceEvent::StateTimeout,
                     "<state_timeout>", /*corr=*/0, nullptr, 0);
        }
        StateTimeoutMsg<StateT> stm{armed_for, cookie};
        StateT before = h.state;
        EventResult<StateT> r = self->handle_event(before, stm, h.data);
        using K = typename EventResult<StateT>::Kind;
        switch (r.kind) {
        case K::Keep:
            break;
        case K::KeepResetTimeout:
            if (r.state_timeout_ms.has_value()) {
                arm_state_timeout_(*self, before, *r.state_timeout_ms);
            }
            break;
        case K::Transition: {
            if (h.state_timer.valid()) {
                self->timers().cancel_timer(std::move(h.state_timer));
                h.state_timer = TimerRef{};
            }
            h.expected_cookie++;
            h.state = r.new_state;
            if (tr2.enabled()) {
                tr2.emit(::demo::runtime::TraceEvent::StateTransition,
                         "<state_timeout>", /*corr=*/0, nullptr, 0);
            }
            while (!h.postponed.empty()) {
                auto fn = std::move(h.postponed.front());
                h.postponed.pop_front();
                base->enqueue(std::move(fn));
            }
            self->on_enter(h.state, before, h.data);
            if (r.state_timeout_ms.has_value()) {
                arm_state_timeout_(*self, h.state, *r.state_timeout_ms);
            }
            break;
        }
        case K::Postpone:
            std::fprintf(stderr,
                "[%s] postpone() from a state-timeout handler is a no-op; "
                "use keep_state_and_reset_timeout to re-arm.\n",
                Derived::kNodeName);
            break;
        case K::Halt:
        case K::HaltWithError: {
            int code = (r.kind == K::HaltWithError) ? 1 : 0;
            self->terminate(r.halt_reason.c_str(), h);
            std::fflush(stdout);
            std::fflush(stderr);
            std::_Exit(code);
        }
        }
    });
}

}  // namespace runtime
}  // namespace demo

// theia::runtime::TimerService — erlang:send_after / cancel_timer.
//
// send_after IS a (deferred) message send. The Erlang spec literally
// reads: "after TimeMs ms → send Msg to Dest". When the timer fires,
// the message lands in Dest's mailbox exactly as if `Dest ! Msg` had
// been done at that moment; Dest's handle_info(Msg, State) runs —
// same callback that handles any non-OTP message that arrives via `!`.
//
// Mirrors the Erlang BIFs (up/otp/erts/preloaded/src/erlang.erl):
//
//   erlang:send_after(Time, Dest, Msg) → TimerRef
//   erlang:cancel_timer(TimerRef) → Time | false
//
// Implementation:
//
//   * Single background thread holds a min-heap of pending timers.
//     Sleeps on a condition_variable until either the next deadline
//     or a wakeup (new timer added / cancellation / shutdown).
//   * Each timer has an atomic state machine plus a per-timer mutex
//     used by strict cancel to wait out a concurrent fire.
//   * Delivery routes through post_info(dest, msg) — the message
//     lands on the destination's handle_info(const char*, State&).
//
// Strict cancel semantics: cancel_timer returns ONLY after the timer
// is either fully delivered (-1) or definitively suppressed (>=0).
// Caller never has to guard against a stale wakeup from a cancelled
// timer.

#pragma once

#include "GenServer.hh"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace theia {
namespace runtime {

class TimerService;

// One scheduled timer. Shared between TimerService's heap and the
// TimerRef the caller holds — both keep the shared_ptr alive across
// the cancel/fire race. Fields are conceptually private; only
// TimerService and the TimerRef accessor methods touch them.
//
// A timer carries EITHER a message-to-send (dest + msg, used by
// send_after) OR a generic action (used by send_after_lambda for
// the request/timeout race in send_request_with_timeout). Exactly
// one is populated; the fire path picks based on whether `action`
// is set.
struct TimerEntry {
    std::chrono::steady_clock::time_point deadline;
    GenServerBase*  dest{nullptr};
    std::string     msg;
    std::function<void()> action;
    enum class State { Pending, Firing, Done, Cancelled };
    std::atomic<State> state{State::Pending};
    std::mutex     fire_mu;  // taken by both fire and strict cancel
    uint64_t       seq;      // tie-breaker for equal deadlines
};

// Opaque handle returned by send_after; passed to cancel_timer.
class TimerRef {
public:
    TimerRef() = default;
    bool valid() const noexcept { return entry_ != nullptr; }

private:
    friend class TimerService;
    explicit TimerRef(std::shared_ptr<TimerEntry> e)
        : entry_(std::move(e)) {}
    std::shared_ptr<TimerEntry> entry_;
};

class TimerService {
public:
    TimerService();
    ~TimerService();

    TimerService(const TimerService&) = delete;
    TimerService& operator=(const TimerService&) = delete;

    // Schedule `msg` for delivery to `dest`'s handle_info after
    // `delay_ms`. Returns a TimerRef the caller can cancel.
    TimerRef send_after(int delay_ms,
                         GenServerBase& dest,
                         std::string msg);

    // Schedule a generic action to run after `delay_ms`. The action is
    // invoked on the timer thread (briefly — it should be short and
    // non-blocking; long work belongs in a target gen_server). Used by
    // send_request_with_timeout for the timeout race without spawning
    // a dedicated thread per call.
    TimerRef send_after_lambda(int delay_ms, std::function<void()> action);

    // Strict cancel. Returns:
    //   * remaining ms (>= 0) if the timer was still pending,
    //   * -1 if the timer had already fired (and the message has been
    //     delivered to the destination's mailbox).
    // Blocks if the timer is currently being fired so the caller can
    // be certain about delivery state on return.
    int cancel_timer(TimerRef ref);

private:
    struct Cmp {
        bool operator()(const std::shared_ptr<TimerEntry>& a,
                         const std::shared_ptr<TimerEntry>& b) const noexcept {
            if (a->deadline != b->deadline) return a->deadline > b->deadline;
            return a->seq > b->seq;
        }
    };

    void loop_();

    std::atomic<bool>       running_{true};
    std::atomic<uint64_t>   next_seq_{1};
    std::mutex              mu_;
    std::condition_variable cv_;
    std::priority_queue<
        std::shared_ptr<TimerEntry>,
        std::vector<std::shared_ptr<TimerEntry>>,
        Cmp>                heap_;
    std::thread             thread_;
};

// Free-function convenience wrappers — mirror erlang:send_after /
// cancel_timer. They take the service explicitly (we don't hide it
// behind a global; ownership is the caller's main()).
inline TimerRef send_after(TimerService& svc, int delay_ms,
                            GenServerBase& dest, std::string msg) {
    return svc.send_after(delay_ms, dest, std::move(msg));
}

inline int cancel_timer(TimerService& svc, TimerRef ref) {
    return svc.cancel_timer(std::move(ref));
}

// ---- send_request_with_timeout ------------------------------------------
//
// Async call that auto-routes its result to the caller's own mailbox.
// Two paths race against each other:
//
//   (a) server runs handle_call, posts a "deliver reply" lambda
//       onto the caller's mailbox;
//   (b) timer fires after `timeout_ms`, posts a "deliver timeout"
//       lambda onto the caller's mailbox.
//
// Whichever path wins the shared atomic flag dispatches into the
// matching caller-side callback. The loser, when its lambda runs,
// observes the flag is already set and silently no-ops.
//
// Caller-side callbacks invoked (overloaded on Reply / Act / State):
//   void Caller::handle_call_result(const Reply&, const Act&, State&);
//   void Caller::handle_call_timeout(const Act&, State&);
//
// The flag is held by shared_ptr so the two lambdas can capture it
// independently and the storage outlives both. This is the C++ shape
// of OTP's "send_request + arm a timer + whichever arrives first wins".

template <typename Reply, typename Server, typename Req, typename Act,
          typename Caller>
void send_request_with_timeout(Server& server,
                                Req req,
                                Act act,
                                int timeout_ms,
                                Caller& caller,
                                TimerService& timers) {
    // Renamed enum members so they don't shadow the Reply template
    // param above.
    enum class Outcome : int { None = 0, GotReply = 1, GotTimeout = 2 };
    auto winner = std::make_shared<std::atomic<int>>(0);

    // Path (a): enqueue the call on the server. When handle_call
    // returns, post a "reply arrived" lambda onto the caller's
    // mailbox. The lambda checks the winner flag on the caller's own
    // thread, so handle_call_result runs serialized with other caller
    // handlers — no locks needed inside it.
    auto act_copy_a = act;
    server.enqueue([req = std::move(req),
                    act = std::move(act_copy_a),
                    winner,
                    caller_ptr = &caller](GenServerBase* base) mutable {
        try {
            auto* self = static_cast<Server*>(base);
            Reply r = self->handle_call(req, self->state());

            // Try to win. Only the first wins; the loser is suppressed.
            int expected = static_cast<int>(Outcome::None);
            if (!winner->compare_exchange_strong(
                    expected, static_cast<int>(Outcome::GotReply))) {
                return;  // timeout already won — drop reply
            }
            // Post a typed-callback lambda onto the caller's mailbox.
            caller_ptr->enqueue([r = std::move(r),
                                  act = std::move(act),
                                  caller_ptr](GenServerBase*) mutable {
                caller_ptr->handle_call_result(r, act, caller_ptr->state());
            });
        } catch (...) {
            // Treat exceptions like a (lost) reply: route to error
            // handler if we win the race, otherwise drop.
            int expected = static_cast<int>(Outcome::None);
            if (!winner->compare_exchange_strong(
                    expected, static_cast<int>(Outcome::GotReply))) {
                return;
            }
            std::string what;
            try { throw; }
            catch (const std::exception& e) { what = e.what(); }
            catch (...) { what = "unknown exception"; }
            caller_ptr->enqueue([what = std::move(what),
                                  act = std::move(act),
                                  caller_ptr](GenServerBase*) mutable {
                caller_ptr->handle_call_error(what, act, caller_ptr->state());
            });
        }
    });

    // Path (b): arm a timer. On fire, the timer posts a string message
    // to the caller's mailbox; from there we re-route to the typed
    // handle_call_timeout overload by carrying the Act in a side-band
    // closure. We bypass post_info / handle_info to keep the Act typed.
    auto act_copy_b = act;
    auto fire_lambda = std::make_shared<std::function<void()>>(
        [winner, act = std::move(act_copy_b),
         caller_ptr = &caller]() mutable {
            int expected = static_cast<int>(Outcome::None);
            if (!winner->compare_exchange_strong(
                    expected, static_cast<int>(Outcome::GotTimeout))) {
                return;  // reply already won — drop timeout
            }
            caller_ptr->enqueue([act = std::move(act),
                                  caller_ptr](GenServerBase*) mutable {
                caller_ptr->handle_call_timeout(act, caller_ptr->state());
            });
        });

    // Arm the timer on the shared service rather than spawning a
    // throwaway thread. send_after_lambda runs the action on the
    // timer thread (very brief — it just CASes the winner flag and
    // posts to the caller's mailbox).
    timers.send_after_lambda(timeout_ms,
        [fire_lambda]() { (*fire_lambda)(); });
}

// ---- process-wide TimerService accessor ----------------------------------
//
// Mirrors process_logger(): main constructs the one TimerService and
// publishes a NON-OWNING pointer here once, before nodes start; any node
// thread then reaches it via process_timers() to send_after() /
// cancel_timer() — so a `requires_timers` node needs no ctor injection.
// (TimerService is non-copyable + owns a thread, hence a pointer, not a
// lazily-created shared_ptr like the logger.) Calling process_timers()
// before publish is a wiring bug — the generated main always publishes
// when any node `requires_timers`.
void           set_process_timers(TimerService* timers) noexcept;
TimerService&  process_timers() noexcept;

}  // namespace runtime
}  // namespace theia

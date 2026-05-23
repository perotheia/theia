// SmDaemon — the State Management functional cluster's main actor.
//
// Layered on the auto-generated SmDaemonStateMBase (the FSM transition
// table from package.art's statem block). This file adds the
// side-effect surface: a subscriber table for SmStateStream + the
// broadcast logic in on_enter.
//
// "Cooperation": per docs/autosar/services/sm.md §3.B, SM is a CLIENT
// of EM/COM/UCM — it broadcasts state changes; each partner reacts
// according to its own policy:
//
//   EXEC  receives SmStateMsg → switches Function Group state
//   COM   receives SmStateMsg → enables / disables network bindings
//   UCM   receives SmStateMsg → pauses / resumes update flashing
//
// The model is broadcast-once / subscribe-many. SmDaemon doesn't hold
// per-partner logic — every partner is just one entry in subscribers_.
// New partners (DM, NM, future) subscribe at startup and don't need
// SmDaemon changes. This matches AUTOSAR's "Notifier" pattern (§3.A).

#pragma once

#include "sm/SmDaemonStateMBase.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <vector>

namespace system_services_sm {

// One subscriber. A free-function that takes the SmStateMsg by const
// ref and delivers it however the subscriber wants — in-process,
// over TIPC, over gRPC. Keeps the daemon free of dispatch flavor:
// the caller-side decides how to ship the message.
using Subscriber = std::function<void(const SmStateMsg&)>;

class SmDaemon : public SmDaemonStateMBase {
public:
    static constexpr const char* kNodeName = "sm";

    // Register a subscriber. Called at startup by main() — once for
    // each partner FC. Returns a handle id so the subscriber can
    // unregister later (subscribe-while-running is allowed but most
    // partners subscribe once and stay).
    uint32_t subscribe(Subscriber s) {
        std::lock_guard<std::mutex> lk(sub_mu_);
        uint32_t id = next_sub_id_++;
        subscribers_.push_back({id, std::move(s)});
        return id;
    }

    void unsubscribe(uint32_t id) {
        std::lock_guard<std::mutex> lk(sub_mu_);
        subscribers_.erase(
            std::remove_if(subscribers_.begin(), subscribers_.end(),
                           [id](const Entry& e) { return e.id == id; }),
            subscribers_.end());
    }

    // ---- FSM side-effect hook ----------------------------------------
    //
    // Runs AFTER every committed transition (and once at init, with
    // new_s == old_s). The base ensures we cannot transition_to from
    // here (return type is void). We:
    //
    //   1. Mutate the FSM's data (SmStateMsg) — `state` field tracks
    //      the FSM, `ts_ns` records when the transition committed.
    //   2. Snapshot subscribers under the lock + release before invoking
    //      so a subscriber that takes a long time (or calls back into
    //      sm) cannot deadlock the lock.
    //   3. Fan out to each. Subscriber callbacks run on the SM thread
    //      (this on_enter runs there). A subscriber that wants
    //      mailbox-isolation should arrange a cast() inside its own
    //      callback — that's the partner's choice.
    //
    // Subscriber failures are NOT propagated — one slow subscriber
    // can't stall the FSM. Subscribers MUST be best-effort.
    void on_enter(SmDaemonState new_s, SmDaemonState /*old_s*/,
                   SmStateMsg& d) {
        d.state = new_s;
        d.ts_ns = now_ns_();

        std::vector<Entry> snap;
        {
            std::lock_guard<std::mutex> lk(sub_mu_);
            snap = subscribers_;
        }
        for (const auto& e : snap) {
            try { e.fn(d); }
            catch (...) {
                std::fprintf(stderr,
                    "[sm] subscriber %u threw on broadcast — dropping\n",
                    e.id);
            }
        }
    }

    // ---- StateMgmtCtl::RequestMode ----------------------------------
    //
    // External clients call this through the ctl server port (defined
    // in package.art). We map the requested target state into the
    // matching FSM event. The .art declares this as
    // `operation RequestMode(in r:SmRequest) returns SmEmpty`, so
    // handle_call returns SmEmpty.
    //
    // We could return early-rejection here (e.g. for unknown targets),
    // but today the FSM silently keeps_state if the request lands in
    // a state that doesn't accept it — same as Erlang's "no clause
    // matches".
    using Holder = demo::runtime::GenStateMHolder<SmDaemonState, SmStateMsg>;
    SmEmpty handle_call(const SmRequest& req, Holder& /*h*/) {
        switch (req.target) {
        case SmState_SHUTDOWN:
            demo::runtime::post_event(*this, ShutdownRequest{});
            break;
        case SmState_UPDATE:
            demo::runtime::post_event(*this, UpdateRequest{});
            break;
        case SmState_RUNNING:
            // RUNNING is reached from STARTING via StartupComplete or
            // from UPDATE via UpdateComplete — not via a direct
            // user-driven request. Drop with a log to surface a
            // misuse.
            std::fprintf(stderr,
                "[sm] RequestMode(RUNNING) — RUNNING is reached via "
                "internal events; request ignored\n");
            break;
        default:
            // OFF / STARTING / DEGRADED are likewise internal-only
            // landing states; reject.
            std::fprintf(stderr,
                "[sm] RequestMode(target=%u) — not a client-driven "
                "target; request ignored\n",
                static_cast<unsigned>(req.target));
            break;
        }
        return SmEmpty{};
    }

private:
    struct Entry { uint32_t id; Subscriber fn; };

    using GenStateMHolder = demo::runtime::GenStateMHolder<SmDaemonState,
                                                            SmStateMsg>;

    static uint64_t now_ns_() {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    }

    std::mutex          sub_mu_;
    uint32_t            next_sub_id_{1};
    std::vector<Entry>  subscribers_;
};

}  // namespace system_services_sm

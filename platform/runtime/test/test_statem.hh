// Test fixtures for GenStateM.
//
// Three FSMs, each minimal and self-contained (no nanopb, no .art):
//
//   TrafficLight    — 3 states (Red/Green/Yellow), cycles on state
//                     timeouts. Exercises auto-transitions via 0ms
//                     timeouts and the cookie/cancel discipline.
//   RetryEscalator  — 2 states (Trying/Failed). Retry event keeps
//                     state but increments a counter; after N retries
//                     within budget the next retry escalates to Failed.
//                     Also tests postpone: a Resume event sent in
//                     Failed is postponed until we get back to Trying.
//   DoorLock        — 3 states (Locked/Unlocking/Unlocked) + guard
//                     conditions. Tests that derived class can
//                     override handle_event() with conditional logic.

#pragma once

#include "GenStateM.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace test_statem {

namespace rt = theia::runtime;

// ---- TrafficLight ------------------------------------------------------

enum class Light { Red, Green, Yellow };

struct TrafficData {
    std::vector<Light> history;  // grows on every on_enter
    std::atomic<int>   ticks{0}; // increments on every state timeout
};

// One single-event-type FSM driven entirely by state timeouts.
struct TLBoot {};   // sent once to kick things off

class TrafficLight
    : public rt::GenStateM<TrafficLight, Light, TrafficData> {
public:
    static constexpr const char* kNodeName = "TrafficLight";

    Light init(TrafficData& /*d*/) { return Light::Red; }

    // External wake — caller sends one TLBoot to start the cycle.
    // We treat receipt-of-Boot-in-Red as "begin cycling": fire a
    // 30ms timeout in Red. After that, state timeouts drive the
    // Red→Green→Yellow→Red rotation autonomously.
    rt::EventResult<Light> handle_event(Light s, const TLBoot&,
                                          TrafficData&) {
        if (s == Light::Red)
            return rt::keep_state_and_reset_timeout<Light>(30);
        return rt::keep_state<Light>();
    }

    rt::EventResult<Light> handle_event(Light s,
                                          const rt::StateTimeoutMsg<Light>&,
                                          TrafficData& d) {
        d.ticks.fetch_add(1);
        switch (s) {
        case Light::Red:
            return rt::transition_to<Light>(Light::Green, 30);
        case Light::Green:
            return rt::transition_to<Light>(Light::Yellow, 30);
        case Light::Yellow:
            return rt::transition_to<Light>(Light::Red, 30);
        }
        return rt::keep_state<Light>();
    }

    void on_enter(Light new_s, Light /*old_s*/, TrafficData& d) {
        d.history.push_back(new_s);
    }
};

// ---- RetryEscalator ----------------------------------------------------

enum class Retry { Trying, Failed };

struct RetryData {
    int  retry_count = 0;
    int  resume_count = 0;
    bool escalated = false;
};

struct DoRetry {};
struct Resume  {};

class RetryEscalator
    : public rt::GenStateM<RetryEscalator, Retry, RetryData> {
public:
    static constexpr const char* kNodeName = "RetryEscalator";
    static constexpr int kMaxRetries = 3;

    // Make the base-class handle_event(StateTimeoutMsg) visible — our
    // derived overloads otherwise hide it (C++ name hiding).
    using rt::GenStateM<RetryEscalator, Retry, RetryData>::handle_event;

    Retry init(RetryData&) { return Retry::Trying; }

    rt::EventResult<Retry> handle_event(Retry s, const DoRetry&,
                                          RetryData& d) {
        if (s == Retry::Trying) {
            d.retry_count++;
            if (d.retry_count >= kMaxRetries) {
                d.escalated = true;
                return rt::transition_to<Retry>(Retry::Failed);
            }
            return rt::keep_state<Retry>();
        }
        // Got DoRetry in Failed — no semantic meaning; ignore.
        return rt::keep_state<Retry>();
    }

    rt::EventResult<Retry> handle_event(Retry s, const Resume&,
                                          RetryData& d) {
        if (s == Retry::Failed) {
            // Postpone — handle once we're back in Trying.
            return rt::postpone<Retry>();
        }
        // Got Resume in Trying.
        d.resume_count++;
        return rt::keep_state<Retry>();
    }
};

// ---- DoorLock ---------------------------------------------------------

enum class Door { Locked, Unlocking, Unlocked };

struct DoorData {
    bool has_keycard = false;
    int  reject_count = 0;
    int  unlock_count = 0;
};

struct PresentCard {};
struct UnlockDone  {};
struct InsertKey   { bool valid; };

class DoorLock
    : public rt::GenStateM<DoorLock, Door, DoorData> {
public:
    static constexpr const char* kNodeName = "DoorLock";

    Door init(DoorData&) { return Door::Locked; }

    // Guard: only transition if the keycard has been registered.
    rt::EventResult<Door> handle_event(Door s, const PresentCard&,
                                         DoorData& d) {
        if (s != Door::Locked) return rt::keep_state<Door>();
        if (!d.has_keycard) {
            d.reject_count++;
            return rt::keep_state<Door>();
        }
        return rt::transition_to<Door>(Door::Unlocking, 50);
    }

    // InsertKey sets the keycard flag (acts as a "registration" event).
    rt::EventResult<Door> handle_event(Door /*s*/, const InsertKey& e,
                                         DoorData& d) {
        d.has_keycard = e.valid;
        return rt::keep_state<Door>();
    }

    rt::EventResult<Door> handle_event(Door s,
                                         const rt::StateTimeoutMsg<Door>&,
                                         DoorData& d) {
        if (s == Door::Unlocking) {
            d.unlock_count++;
            return rt::transition_to<Door>(Door::Unlocked);
        }
        return rt::keep_state<Door>();
    }

    rt::EventResult<Door> handle_event(Door s, const UnlockDone&,
                                         DoorData&) {
        if (s == Door::Unlocking)
            return rt::transition_to<Door>(Door::Unlocked);
        return rt::keep_state<Door>();
    }
};

}  // namespace test_statem

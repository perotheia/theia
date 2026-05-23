// SmDaemon — integration test driving the cooperation surface.
//
// Asserts the broadcast contract described in
// docs/autosar/services/sm.md §3.B: every committed transition fires
// one SmStateMsg to every subscriber, carrying the post-transition
// state. The test stands in for the eventual EXEC / COM / UCM
// subscribers — we register three callbacks, each emulating one
// partner, and assert every partner sees the same sequence.
//
// In-process today (LocalRef / direct subscribe). When SM gets a
// TIPC publisher, this same test grows a cross-process variant
// alongside.

#include "sm/sm_daemon.hh"

#include "TimerService.hh"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace system_services_sm;

namespace {

struct TestStat {
    int total = 0;
    int passed = 0;
    std::vector<std::string> failures;
};

#define EXPECT(cond, msg) \
    do { if (!(cond)) return std::string(msg) + " (" #cond ")"; } while (0)

#define CASE(stat, name) run_case(stat, #name, []() -> std::string

static void run_case(TestStat& stat, const char* name,
                      std::string (*body)()) {
    ++stat.total;
    std::printf("• %-40s ", name);
    std::fflush(stdout);
    std::string err;
    try { err = body(); }
    catch (const std::exception& e) { err = std::string("threw: ") + e.what(); }
    catch (...)                     { err = "threw: unknown"; }
    if (err.empty()) {
        ++stat.passed;
        std::printf("PASS\n");
    } else {
        stat.failures.emplace_back(std::string(name) + ": " + err);
        std::printf("FAIL — %s\n", err.c_str());
    }
}

// A subscriber that records every SmStateMsg it sees. Mimics a
// partner FC (EXEC / COM / UCM) that observes platform state.
struct Recorder {
    std::mutex mu;
    std::vector<SmState> states;

    void operator()(const SmStateMsg& m) {
        std::lock_guard<std::mutex> lk(mu);
        states.push_back(m.state);
    }

    std::vector<SmState> snapshot() {
        std::lock_guard<std::mutex> lk(mu);
        return states;
    }
};

static void pump(int ms = 20) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ---- CASES ---------------------------------------------------------

static std::string case_init_fires_first_on_enter() {
    demo::runtime::TimerService timers;
    SmDaemon sm;
    Recorder r;
    sm.subscribe(std::ref(r));
    sm.start_statem(timers);
    pump();
    auto seen = r.snapshot();
    EXPECT(!seen.empty(), "init must fire at least one on_enter");
    EXPECT(seen[0] == SmState_OFF, "first broadcast should be OFF");
    sm.stop();
    return {};
}

static std::string case_full_lifecycle_to_shutdown() {
    // Drive OFF → STARTING → RUNNING → SHUTDOWN. Confirm the
    // subscriber sees every transition, in order, exactly once.
    demo::runtime::TimerService timers;
    SmDaemon sm;
    Recorder r;
    sm.subscribe(std::ref(r));
    sm.start_statem(timers);
    pump();

    demo::runtime::post_event(sm, SystemBoot{});
    pump();
    demo::runtime::post_event(sm, StartupComplete{});
    pump();
    demo::runtime::post_event(sm, ShutdownRequest{});
    pump();

    auto seen = r.snapshot();
    EXPECT(seen.size() == 4,
           "expected 4 on_enter calls (OFF init, STARTING, RUNNING, "
           "SHUTDOWN); got " + std::to_string(seen.size()));
    EXPECT(seen[0] == SmState_OFF,       "history[0]=OFF");
    EXPECT(seen[1] == SmState_STARTING,  "history[1]=STARTING");
    EXPECT(seen[2] == SmState_RUNNING,   "history[2]=RUNNING");
    EXPECT(seen[3] == SmState_SHUTDOWN,  "history[3]=SHUTDOWN");
    sm.stop();
    return {};
}

static std::string case_update_loopback() {
    // RUNNING → UPDATE → RUNNING. Each partner sees both transitions.
    demo::runtime::TimerService timers;
    SmDaemon sm;
    Recorder r;
    sm.subscribe(std::ref(r));
    sm.start_statem(timers);
    pump();
    demo::runtime::post_event(sm, SystemBoot{});
    pump();
    demo::runtime::post_event(sm, StartupComplete{});
    pump();
    demo::runtime::post_event(sm, UpdateRequest{});
    pump();
    demo::runtime::post_event(sm, UpdateComplete{});
    pump();

    auto seen = r.snapshot();
    // OFF (init), STARTING, RUNNING, UPDATE, RUNNING.
    EXPECT(seen.size() == 5, "5 transitions expected, got " +
                                std::to_string(seen.size()));
    EXPECT(seen[3] == SmState_UPDATE,
           "expected UPDATE after UpdateRequest");
    EXPECT(seen[4] == SmState_RUNNING,
           "expected RUNNING after UpdateComplete");
    sm.stop();
    return {};
}

static std::string case_multiple_subscribers_all_see_every_transition() {
    // Three subscribers — stand-ins for EXEC, COM, UCM. All MUST see
    // the same SmStateMsg sequence. This is the broadcast-once /
    // subscribe-many contract from sm.md §3.B.
    demo::runtime::TimerService timers;
    SmDaemon sm;
    Recorder exec_sub, com_sub, ucm_sub;
    sm.subscribe(std::ref(exec_sub));
    sm.subscribe(std::ref(com_sub));
    sm.subscribe(std::ref(ucm_sub));
    sm.start_statem(timers);
    pump();
    demo::runtime::post_event(sm, SystemBoot{});
    pump();
    demo::runtime::post_event(sm, StartupComplete{});
    pump();

    auto e = exec_sub.snapshot();
    auto c = com_sub.snapshot();
    auto u = ucm_sub.snapshot();
    EXPECT(e == c, "EXEC and COM subscribers must see identical history");
    EXPECT(c == u, "COM and UCM subscribers must see identical history");
    EXPECT(e.size() == 3,
           "expected 3 transitions (OFF init, STARTING, RUNNING); got " +
               std::to_string(e.size()));
    sm.stop();
    return {};
}

static std::string case_unsubscribe_stops_delivery() {
    // A subscriber that unsubscribes mid-lifecycle stops receiving.
    // The remaining one still gets every later transition.
    demo::runtime::TimerService timers;
    SmDaemon sm;
    Recorder leaver, keeper;
    uint32_t leaver_id = sm.subscribe(std::ref(leaver));
    sm.subscribe(std::ref(keeper));
    sm.start_statem(timers);
    pump();
    demo::runtime::post_event(sm, SystemBoot{});
    pump();
    // After STARTING, leaver bails.
    sm.unsubscribe(leaver_id);
    demo::runtime::post_event(sm, StartupComplete{});
    pump();
    demo::runtime::post_event(sm, ShutdownRequest{});
    pump();

    auto l = leaver.snapshot();
    auto k = keeper.snapshot();
    EXPECT(l.size() == 2,
           "leaver should see OFF + STARTING only, got " +
               std::to_string(l.size()));
    EXPECT(k.size() == 4,
           "keeper should see all 4, got " + std::to_string(k.size()));
    sm.stop();
    return {};
}

static std::string case_state_timestamps_monotonic() {
    // ts_ns on every broadcast must be monotonically non-decreasing.
    // (Subscribers can rely on this for ordering when reasonable.)
    demo::runtime::TimerService timers;
    SmDaemon sm;
    std::mutex mu;
    std::vector<uint64_t> ts;
    sm.subscribe([&](const SmStateMsg& m) {
        std::lock_guard<std::mutex> lk(mu);
        ts.push_back(m.ts_ns);
    });
    sm.start_statem(timers);
    pump();
    demo::runtime::post_event(sm, SystemBoot{});
    pump();
    demo::runtime::post_event(sm, StartupComplete{});
    pump();
    {
        std::lock_guard<std::mutex> lk(mu);
        EXPECT(ts.size() >= 3, "expected at least 3 broadcasts");
        for (size_t i = 1; i < ts.size(); ++i) {
            EXPECT(ts[i] >= ts[i - 1], "ts_ns must be monotonic");
        }
    }
    sm.stop();
    return {};
}

static std::string case_request_mode_translates_to_event() {
    // handle_call(SmRequest{SHUTDOWN}) issued while RUNNING should
    // ultimately land us in SHUTDOWN. The mapping lives in
    // SmDaemon::handle_call — we use the post_event path
    // synchronously here (handle_call is also reachable via the
    // server port in production).
    //
    // Note: rt::call would route through the GenServer dispatcher;
    // for this in-process test we drive the state machine directly
    // by post_event since that's what RequestMode would do anyway.
    demo::runtime::TimerService timers;
    SmDaemon sm;
    Recorder r;
    sm.subscribe(std::ref(r));
    sm.start_statem(timers);
    pump();
    demo::runtime::post_event(sm, SystemBoot{});
    demo::runtime::post_event(sm, StartupComplete{});
    pump();
    // Stand-in for ctl.RequestMode(SHUTDOWN): same event the daemon's
    // handle_call would post on the FSM.
    demo::runtime::post_event(sm, ShutdownRequest{});
    pump();
    auto seen = r.snapshot();
    EXPECT(seen.back() == SmState_SHUTDOWN,
           "RequestMode(SHUTDOWN) → expected SHUTDOWN as final state");
    sm.stop();
    return {};
}

}  // namespace

int main() {
    TestStat stat;
    CASE(stat, init_fires_first_on_enter) {
        return case_init_fires_first_on_enter();
    });
    CASE(stat, full_lifecycle_to_shutdown) {
        return case_full_lifecycle_to_shutdown();
    });
    CASE(stat, update_loopback) {
        return case_update_loopback();
    });
    CASE(stat, multiple_subscribers_all_see_every_transition) {
        return case_multiple_subscribers_all_see_every_transition();
    });
    CASE(stat, unsubscribe_stops_delivery) {
        return case_unsubscribe_stops_delivery();
    });
    CASE(stat, state_timestamps_monotonic) {
        return case_state_timestamps_monotonic();
    });
    CASE(stat, request_mode_translates_to_event) {
        return case_request_mode_translates_to_event();
    });

    std::printf("\n%d/%d passed\n", stat.passed, stat.total);
    if (!stat.failures.empty()) {
        std::printf("\nFailures:\n");
        for (const auto& f : stat.failures) std::printf("  ✗ %s\n", f.c_str());
        return 1;
    }
    return 0;
}

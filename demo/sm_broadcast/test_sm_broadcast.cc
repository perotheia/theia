// SM → cooperation-partners in-process broadcast smoke.
//
// Stands up sm + the four sm.md §3.B partners (exec/com/ucm/per) in
// one process and validates the broadcast contract: every state
// transition committed by sm lands on every partner via its own
// GenServer mailbox. No TIPC — the wiring is in-process LocalRef-style
// using the subscriber callback that sm.subscribe() already accepts.
//
// What's REAL here:
//   - 5 GenServer daemons, each with its own thread + mailbox.
//   - sm's on_enter fires after every committed transition (the
//     real Phase B / D side-effect path).
//   - Subscriber callbacks capture each partner by reference; the
//     callback enqueues a state-mutator lambda onto the partner's
//     mailbox via the GenServerBase::enqueue primitive. So the
//     partner observes the SmStateMsg ON ITS OWN THREAD — same
//     isolation a TIPC subscriber would get.
//
// What's NOT real:
//   - No TIPC — partners are in-process. Cross-process variant comes
//     later (sm grows a TIPC publisher, partners subscribe via
//     RemoteRef).

#include "GenServer.hh"
#include "TimerService.hh"

#include "sm/sm_daemon.hh"
#include "lib/ExecDaemon.hh"
#include "lib/ComDaemon.hh"
#include "lib/UcmDaemon.hh"
#include "lib/PerDaemon.hh"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace rt   = demo::runtime;
namespace sm   = system_services_sm;
namespace exec = ara::exec;
namespace com  = ara::com;
namespace ucm  = ara::ucm;
namespace per_ = ara::per;

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
    std::printf("• %-50s ", name);
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

static void pump(int ms = 30) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Each partner exposes its observed-SmState via an atomic so the test
// thread can observe deterministically without grabbing the partner's
// mailbox lock. The subscriber callback enqueues a setter lambda onto
// the partner's mailbox (so the partner's thread does the assignment —
// real per-daemon serialization).
template <typename Daemon>
static std::function<void(const sm::SmStateMsg&)>
wire(Daemon& d, std::atomic<int>& obs) {
    return [&d, &obs](const sm::SmStateMsg& m) {
        // Snapshot the value into a local + enqueue.
        sm::SmState s = m.state;
        d.enqueue([&obs, s](rt::GenServerBase*) {
            obs.store(static_cast<int>(s));
        });
    };
}

static std::string case_all_partners_observe_full_lifecycle() {
    rt::TimerService timers;
    sm::SmDaemon         sm_daemon;
    exec::ExecDaemon     exec_daemon;
    com::ComDaemon       com_daemon;
    ucm::UcmDaemon       ucm_daemon;
    per_::PerDaemon      per_daemon;

    // Per-partner observed state. Atomic int so the test thread can
    // wait without locks; -1 means "no broadcast received yet".
    std::atomic<int> obs_exec{-1};
    std::atomic<int> obs_com{-1};
    std::atomic<int> obs_ucm{-1};
    std::atomic<int> obs_per{-1};

    sm_daemon.subscribe(wire(exec_daemon, obs_exec));
    sm_daemon.subscribe(wire(com_daemon,  obs_com));
    sm_daemon.subscribe(wire(ucm_daemon,  obs_ucm));
    sm_daemon.subscribe(wire(per_daemon,  obs_per));

    sm_daemon.start_statem(timers);
    exec_daemon.start();
    com_daemon.start();
    ucm_daemon.start();
    per_daemon.start();
    pump();

    // After init, every partner should see OFF.
    EXPECT(obs_exec.load() == static_cast<int>(sm::SmState_OFF),
           "exec must see OFF on init");
    EXPECT(obs_com.load()  == static_cast<int>(sm::SmState_OFF),
           "com must see OFF on init");
    EXPECT(obs_ucm.load()  == static_cast<int>(sm::SmState_OFF),
           "ucm must see OFF on init");
    EXPECT(obs_per.load()  == static_cast<int>(sm::SmState_OFF),
           "per must see OFF on init");

    // Drive OFF → STARTING → RUNNING.
    rt::post_event(sm_daemon, sm::SystemBoot{});
    pump();
    rt::post_event(sm_daemon, sm::StartupComplete{});
    pump();

    EXPECT(obs_exec.load() == static_cast<int>(sm::SmState_RUNNING),
           "exec must reach RUNNING");
    EXPECT(obs_com.load()  == static_cast<int>(sm::SmState_RUNNING),
           "com must reach RUNNING");
    EXPECT(obs_ucm.load()  == static_cast<int>(sm::SmState_RUNNING),
           "ucm must reach RUNNING");
    EXPECT(obs_per.load()  == static_cast<int>(sm::SmState_RUNNING),
           "per must reach RUNNING");

    // RUNNING → UPDATE → RUNNING.
    rt::post_event(sm_daemon, sm::UpdateRequest{});
    pump();
    EXPECT(obs_exec.load() == static_cast<int>(sm::SmState_UPDATE),
           "exec must see UPDATE");
    EXPECT(obs_ucm.load()  == static_cast<int>(sm::SmState_UPDATE),
           "ucm must see UPDATE (this is the partner who cares most)");

    rt::post_event(sm_daemon, sm::UpdateComplete{});
    pump();
    EXPECT(obs_exec.load() == static_cast<int>(sm::SmState_RUNNING),
           "exec returns to RUNNING after UpdateComplete");

    // RUNNING → SHUTDOWN.
    rt::post_event(sm_daemon, sm::ShutdownRequest{});
    pump();
    EXPECT(obs_exec.load() == static_cast<int>(sm::SmState_SHUTDOWN),
           "exec must see SHUTDOWN");

    sm_daemon.stop();
    exec_daemon.stop();
    com_daemon.stop();
    ucm_daemon.stop();
    per_daemon.stop();
    return {};
}

}  // namespace

int main() {
    TestStat stat;
    CASE(stat, all_partners_observe_full_lifecycle) {
        return case_all_partners_observe_full_lifecycle();
    });

    std::printf("\n%d/%d passed\n", stat.passed, stat.total);
    if (!stat.failures.empty()) {
        std::printf("\nFailures:\n");
        for (const auto& f : stat.failures) std::printf("  ✗ %s\n", f.c_str());
        return 1;
    }
    return 0;
}

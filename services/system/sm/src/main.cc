// SM (State Management) — main entry point.
//
// Boots the SmDaemon, attaches a stderr-trace subscriber so transitions
// land in the log, registers heartbeat publication for the supervisor's
// watchdog, blocks on SIGTERM / SIGINT, then halts cleanly.
//
// Today this binary runs in-process only — no TIPC mux, no remote
// subscribers. Partners (EXEC / COM / UCM) will subscribe through
// the future TIPC-publisher wiring; the broadcast surface is already
// here (SmDaemon::subscribe), it just gets fed by main() in this build.

#include "sm/sm_daemon.hh"

// HeartbeatPublisher would normally go here but it pulls in
// HeartbeatReport.pb.h, which is a CMake-generated artifact not
// available in this Bazel build. Once SM gets its proto schema
// wired through Bazel codegen, re-enable the publisher so the
// supervisor's watchdog can detect a wedged FSM.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

namespace {

std::atomic<bool> g_running{true};

void on_signal(int /*sig*/) { g_running.store(false); }

const char* state_name(system_services_sm::SmState s) {
    using namespace system_services_sm;
    switch (s) {
    case SmState_OFF:       return "OFF";
    case SmState_STARTING:  return "STARTING";
    case SmState_RUNNING:   return "RUNNING";
    case SmState_DEGRADED:  return "DEGRADED";
    case SmState_UPDATE:    return "UPDATE";
    case SmState_SHUTDOWN:  return "SHUTDOWN";
    }
    return "?";
}

}  // namespace

int main() {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    using namespace system_services_sm;

    demo::runtime::TimerService timers;
    SmDaemon sm;

    // Stderr-log subscriber: every transition shows up in the
    // supervisor's per-child log capture. This is the "Notifier"
    // half of AUTOSAR §3.A documented in docs/autosar/services/sm.md.
    sm.subscribe([](const SmStateMsg& m) {
        std::fprintf(stderr, "[sm] → %s @ %llu\n",
                     state_name(m.state),
                     static_cast<unsigned long long>(m.ts_ns));
        std::fflush(stderr);
    });

    // Start the FSM. init() returns OFF, on_enter fires once with
    // new==old==OFF, which trips the first subscriber callback.
    sm.start_statem(timers);

    // (Heartbeat to supervisor's watchdog: deferred — see include
    // comment above. We re-enable when SM's proto schema lands and
    // HeartbeatReport.pb.h becomes visible to the Bazel build.)

    // Kick off the FSM. In a wired-up rig the first SystemBoot would
    // come from EXEC after the supervisor signals "machine up";
    // standalone, we self-trigger so the cluster reaches RUNNING.
    demo::runtime::post_event(sm, SystemBoot{});

    // Park until SIGTERM. The supervisor sends SIGTERM on shutdown,
    // which trips the signal handler → main loop exits → sm.stop()
    // drains and runs terminate() on the SM thread.
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Final transition out: post ShutdownRequest if we're RUNNING,
    // then PowerOff to land in halt. Best-effort — if we're already
    // shutting down or in a state that doesn't accept these, the
    // FSM keeps_state.
    demo::runtime::post_event(sm, ShutdownRequest{});
    demo::runtime::post_event(sm, PowerOff{});

    // halt() inside the FSM calls std::_Exit(0) — control doesn't
    // return here on the orderly-shutdown path. The sm.stop() below
    // is a guard for the case where PowerOff doesn't land (FSM not
    // in SHUTDOWN), to make the binary exit cleanly anyway.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sm.stop("signal");
    return 0;
}

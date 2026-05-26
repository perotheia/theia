// HAND-OWNED main slice for sm.
//
// Started from the gen-app statem-variant template, then hand-edited to
// wire the two-node SM in-process: SmGate forwards lifecycle messages it
// receives over TIPC into SmDaemon's FSM via post_event(). gen-app skips
// main.cc only with --force suppressed for sm — DO re-apply this wiring
// if regenerated (the LocalRef<SmDaemon> publish + sm_statem_ref()).
// source: services/system/sm/package.art

#include "lib/SmDaemon.hh"
#include "lib/SmGate.hh"

#include "Logger.hh"     // parse_log_level / process_logger
#include "NodeRef.hh"    // demo::runtime::LocalRef

#include "TipcMux.hh"    // config-service receiver for reporting nodes (#386)


#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace ara::sm {

// The statem peer the gate forwards into. SmGate_handlers.cc declares
// `LocalRef<SmDaemon>& sm_statem_ref();` and calls post_event on its
// target; we define it here as a function-local static so the gate and
// main share one reference. main wires its target once SmDaemon exists.
demo::runtime::LocalRef<SmDaemon>& sm_statem_ref() {
    static demo::runtime::LocalRef<SmDaemon> ref;
    return ref;
}

}  // namespace ara::sm

namespace {

std::atomic<bool> g_running{true};

void on_signal(int /*sig*/) { g_running.store(false); }

}  // namespace

int main() {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    using namespace ara::sm;

    // Process-wide logger. Pick up THEIA_LOG_LEVEL from the env
    // (supervisor sets it from executor.json's per-child env map,
    // sourced from Process.log_level on the rig). Defaults to Info
    // when unset or unparseable. set_process_logger publishes it so a
    // reporting node's config service can apply a live ConfigureLogLevel
    // push (#386) via process_logger().set_level on the node thread.
    auto logger = MakeContextLogger();
    if (const char* lvl = std::getenv("THEIA_LOG_LEVEL")) {
        logger->set_level(::platform::runtime::parse_log_level(lvl));
    }
    ::platform::runtime::set_process_logger(logger);
    (void)logger;

    demo::runtime::TimerService timers;
    (void)timers;  // wired into statem nodes' send_after

    // Config-service receiver (#386): reporting nodes register_cast the
    // supervisor's LogLevelPush, applied by GenServer's base handler.
    demo::runtime::TipcMux config_mux;


    SmDaemon sm_daemon;
    sm_daemon.start_statem(timers);
    std::fprintf(stderr,
                 "[sm_daemon] up — TIPC type=0x%x instance=%u; "
                 "statem initial=OFF\n",
                 SmDaemon::kTipcType, SmDaemon::kTipcInstance);

    // Publish the statem peer so SmGate's handlers can post_event into
    // the FSM in-process. Must be wired before SmGate starts receiving.
    sm_statem_ref() = demo::runtime::LocalRef<SmDaemon>(sm_daemon);

    if (auto* sm_daemon_cfg = config_mux.bind_node(
            sm_daemon, SmDaemon::kTipcType,
            SmDaemon::kTipcInstance)) {
        config_mux.register_cast<platform_runtime_LogLevelPush>(
            sm_daemon_cfg, sm_daemon);
        // Trace control (#403): supervisor pushes TraceControlPush to flip
        // this node's Tracer kind filter — same path as LogLevelPush.
        config_mux.register_cast<platform_runtime_TraceControlPush>(
            sm_daemon_cfg, sm_daemon);
        // Receiver ports (#387): register the node's declared inbound
        // types so a real peer — or a robot-test inject via services/com
        // — lands on the same handle_call / handle_cast path. clientServer
        // ops → register_call; senderReceiver `in` data → register_cast.
        config_mux.register_call<SmRequest, SmEmpty>(
            sm_daemon_cfg, sm_daemon);
    } else {
        std::fprintf(stderr,
                     "[sm_daemon] WARN: config service bind failed; "
                     "live log-level push + signal inject disabled\n");
    }


    SmGate sm_gate;
    sm_gate.start();
    std::fprintf(stderr, "[sm_gate] up — TIPC type=0x%x instance=%u\n",
                 SmGate::kTipcType, SmGate::kTipcInstance);

    if (auto* sm_gate_cfg = config_mux.bind_node(
            sm_gate, SmGate::kTipcType,
            SmGate::kTipcInstance)) {
        config_mux.register_cast<platform_runtime_LogLevelPush>(
            sm_gate_cfg, sm_gate);
        // Trace control (#403): supervisor pushes TraceControlPush to flip
        // this node's Tracer kind filter — same path as LogLevelPush.
        config_mux.register_cast<platform_runtime_TraceControlPush>(
            sm_gate_cfg, sm_gate);
        // Receiver ports (#387): register the node's declared inbound
        // types so a real peer — or a robot-test inject via services/com
        // — lands on the same handle_call / handle_cast path. clientServer
        // ops → register_call; senderReceiver `in` data → register_cast.
        config_mux.register_cast<SystemBoot>(sm_gate_cfg, sm_gate);
        config_mux.register_cast<StartupComplete>(sm_gate_cfg, sm_gate);
        config_mux.register_cast<ShutdownRequest>(sm_gate_cfg, sm_gate);
        config_mux.register_cast<UpdateRequest>(sm_gate_cfg, sm_gate);
        config_mux.register_cast<UpdateComplete>(sm_gate_cfg, sm_gate);
        config_mux.register_cast<RetryStartup>(sm_gate_cfg, sm_gate);
        config_mux.register_cast<PowerOff>(sm_gate_cfg, sm_gate);
    } else {
        std::fprintf(stderr,
                     "[sm_gate] WARN: config service bind failed; "
                     "live log-level push + signal inject disabled\n");
    }



    config_mux.start();


    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }


    config_mux.stop();


    sm_daemon.stop("signal");

    sm_gate.stop("signal");

    return 0;
}

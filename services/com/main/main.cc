// HAND-OWNED. source: services/system/com/package.art
//
// FIRST-TIME-ONLY SCAFFOLD origin: `artheia gen-app --kind fc`. The thread
// executor below started as the gen-app main template; it is now HAND-OWNED
// because it register_casts the supervisor's #429 topo-pair firehose stream
// (SnapshotBegin/NodeEdge/NodeState/SnapshotEnd) onto ComDaemon's config_mux
// and feeds SupFirehose — the firehose now arrives over the STANDARD runtime
// transport, not the legacy tipc_uplink (#432). The HAND-OWNED banner makes
// the fc_regen_stability selftest skip this file's byte-diff.

#include "lib/ComDaemon.hh"
#include "lib/ComGrpcProxy.hh"

#include "TimerService.hh"
#include "Logger.hh"     // parse_log_level / MakeContextLogger / process_logger

#include "TipcMux.hh"    // config-service receiver for reporting nodes (#386)

// #432 — register the supervisor's #429 topo-pair firehose stream casts onto
// ComDaemon's binding. The nanopb decode + the SupFirehose reassembly are
// CONFINED to impl/sup_firehose_register.cc (which owns the supervisor nanopb
// structs) + impl/sup_firehose.cc (which owns the libprotobuf TreeSnapshot) —
// so main.cc never sees a supervisor .pb.h and the same-basename nanopb /
// libprotobuf headers never meet on this TU's include path. main.cc passes
// only the opaque NodeBinding*.
#include "impl/sup_firehose_register.hpp"


#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

std::atomic<bool> g_running{true};

void on_signal(int /*sig*/) { g_running.store(false); }

}  // namespace

int main() {
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    using namespace ara::com;

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
    (void)logger;  // available for user handlers via RuntimeContext

    demo::runtime::TimerService timers;
    (void)timers;  // wired into the daemon if it needs send_after

    // Config-service receiver (#386). A reporting node binds its TIPC
    // name and register_cast's the supervisor's control push; the cast
    // lands in GenServer's base handle_cast(LogLevelPush) on the node
    // thread. Same standard GwMessageHeader path as any app message —
    // no bespoke control frame.
    demo::runtime::TipcMux config_mux;


    ComDaemon com_daemon;
    com_daemon.start();
    std::fprintf(stderr, "[com_daemon] up — TIPC type=0x%x instance=%u\n",
                 ComDaemon::kTipcType, ComDaemon::kTipcInstance);

    if (auto* com_daemon_cfg = config_mux.bind_node(
            com_daemon, ComDaemon::kTipcType,
            ComDaemon::kTipcInstance)) {
        config_mux.register_cast<platform_runtime_LogLevelPush>(
            com_daemon_cfg, com_daemon);
        // Trace control (#403): supervisor pushes TraceControlPush to flip
        // this node's Tracer kind filter — same path as LogLevelPush.
        config_mux.register_cast<platform_runtime_TraceControlPush>(
            com_daemon_cfg, com_daemon);
        // Receiver ports (#387): register the node's declared inbound
        // types so a real peer — or a robot-test inject via services/com
        // — lands on the same handle_call / handle_cast path. clientServer
        // ops → register_call; senderReceiver `in` data → register_cast.
        config_mux.register_call<ComEmpty, ComEmpty>(
            com_daemon_cfg, com_daemon);
        config_mux.register_call<NetworkBindingRequest, ComEmpty>(
            com_daemon_cfg, com_daemon);
        // #432 — the supervisor's #429 topo-pair firehose stream lands on
        // ComDaemon's TIPC name; decode + feed SupFirehose (reassembles a
        // TreeSnapshot for the gRPC Subscribe stream). This is what retires
        // the legacy tipc_uplink. Confined to sup_firehose_register.cc.
        services_com::register_firehose_casts(com_daemon_cfg);
    } else {
        std::fprintf(stderr,
                     "[com_daemon] WARN: config service bind failed; "
                     "live log-level push + signal inject disabled\n");
    }


    ComGrpcProxy com_grpc_proxy;
    com_grpc_proxy.start();
    std::fprintf(stderr, "[com_grpc_proxy] up — TIPC type=0x%x instance=%u\n",
                 ComGrpcProxy::kTipcType, ComGrpcProxy::kTipcInstance);



    config_mux.start();


    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }


    config_mux.stop();


    com_daemon.stop("signal");

    com_grpc_proxy.stop("signal");

    return 0;
}

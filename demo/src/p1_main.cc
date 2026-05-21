// Process P1 — the existing demo's heart. Hosts CounterNode (which
// answers Get/Inc from any caller, local or remote) plus the existing
// DriverNode + TickerNode. Also stands up a TipcMux that binds counter
// to its .art tipc address so remote ObserverNode (P2) and
// IncrementerNode (P3) can connect.
//
// The trick worth pointing out: code below makes ZERO distinction
// between local-driver casts and remote-incrementer casts. Both flow
// through counter.handle_cast(Inc, State&). The mux's wire-layer
// decoder turns inbound TIPC bytes into a typed Inc and enqueues onto
// counter's mailbox — exactly what cast(local_ref, Inc{}) does, except
// the message took a TIPC hop on the way.

#include "GenServer.hh"
#include "Logger.hh"
#include "NodeRef.hh"
#include "TimerService.hh"
#include "TipcMux.hh"
#include "counter_node.hh"
#include "demo_codecs.hh"        // RemoteCodec<demo_system_*> specializations
#include "driver_node.hh"
#include "ticker_node.hh"

#include "demo/system/system.pb.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
std::atomic<bool> g_shutdown{false};
void on_sig(int) noexcept { g_shutdown.store(true); }
}

int main() {
    std::signal(SIGINT,  on_sig);
    std::signal(SIGTERM, on_sig);

    auto logger = platform::runtime::MakeConsoleLogger();
    logger->info("=== P1 start (counter + driver + ticker) ===");

    demo::runtime::TimerService timers;

    demo::CounterNode counter(demo::CounterNodeInputs{logger});

    demo::TickerNode  ticker(
        demo::TickerNodeInputs{logger, timers, /*max_ticks=*/30});

    // Driver targets counter locally for both Inc casts and Get calls.
    demo::runtime::LocalRef<demo::CounterNode> counter_ref(counter);
    using DriverT = demo::DriverNode<
        demo::runtime::LocalRef<demo::CounterNode>,
        demo::runtime::LocalRef<demo::CounterNode>>;
    DriverT driver(DriverT::Inputs{
        logger, timers, counter_ref, counter_ref});

    // Open the TIPC mux and register counter for inbound Inc/Get from
    // remote processes. The same register_cast<T>() / register_call<R,Q>()
    // entries that artheia gen-routing will eventually emit per the
    // composition's `connect` lines.
    demo::runtime::TipcMux mux;
    auto* binding = mux.bind_node(counter, /*type=*/0xd0010001u, /*inst=*/0u);
    if (!binding) {
        logger->error("P1: failed to bind counter on TIPC");
        return 1;
    }
    mux.register_cast<demo_system_Inc>(binding, counter);
    mux.register_call<demo_system_Get, demo_system_GetReply>(binding, counter);
    mux.start();

    counter.start();
    driver.start();
    ticker.start();
    driver.kick_off();
    ticker.kick_off();

    // Run until SIGINT or until we've been alive long enough for the
    // 3-process test driver to do its work (default ~6s; bumped by env
    // var for longer manual runs).
    int run_ms = 6000;
    if (const char* env = std::getenv("DEMO_RUN_MS")) {
        run_ms = std::atoi(env);
    }
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(run_ms);
    while (!g_shutdown.load() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    logger->info("=== P1 stopping ===");
    mux.stop();
    ticker.stop();
    driver.stop();
    counter.stop("normal");

    logger->info("=== P1 summary: counter=" +
                 std::to_string(counter.state().counter) +
                 " driver.replies_ok=" +
                 std::to_string(driver.state().replies_ok) + " ===");
    return 0;
}

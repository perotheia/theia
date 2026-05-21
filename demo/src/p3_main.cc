// Process P3 — hosts IncrementerNode. Casts Inc{2} at P1's counter
// every 300ms. No replies expected (cast is fire-and-forget); no mux
// needed beyond the outbound TIPC client.

#include "GenServer.hh"
#include "Logger.hh"
#include "NodeRef.hh"
#include "TimerService.hh"
#include "counter_node.hh"
#include "demo_codecs.hh"
#include "incrementer_node.hh"

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
    logger->info("=== P3 start (incrementer) ===");

    demo::runtime::TimerService timers;

    demo::runtime::RemoteRef<demo::CounterNode, 0xd0010001u, 0u> counter_ref;
    if (!counter_ref.connect(3000)) {
        logger->error("P3: failed to connect to P1's counter");
        return 1;
    }
    logger->info("P3: connected to P1.counter");

    using IncrT = demo::IncrementerNode<decltype(counter_ref)>;
    IncrT incr(IncrT::Inputs{logger, timers, counter_ref});
    incr.start();
    incr.kick_off();

    int run_ms = 5000;
    if (const char* env = std::getenv("DEMO_RUN_MS")) run_ms = std::atoi(env);
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(run_ms);
    while (!g_shutdown.load() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    logger->info("=== P3 stopping ===");
    incr.stop("normal");

    logger->info("=== P3 summary: casts_sent=" +
                 std::to_string(incr.state().casts_sent) + " ===");
    return 0;
}

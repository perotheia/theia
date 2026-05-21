// Process P2 — hosts ObserverNode. Owns a RemoteRef<CounterNode>
// pointing at P1's counter (TIPC 0xd0010001:0). Polls counter.Get
// every 200ms and logs the value.

#include "GenServer.hh"
#include "Logger.hh"
#include "NodeRef.hh"
#include "TimerService.hh"
#include "TipcMux.hh"
#include "counter_node.hh"          // typedef target (we only refer to the type)
#include "demo_codecs.hh"
#include "observer_node.hh"

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
    logger->info("=== P2 start (observer) ===");

    demo::runtime::TimerService timers;

    // RemoteRef to P1's counter. Eager-retry up to 3s.
    demo::runtime::RemoteRef<demo::CounterNode, 0xd0010001u, 0u> counter_ref;
    if (!counter_ref.connect(3000)) {
        logger->error("P2: failed to connect to P1's counter (timeout)");
        return 1;
    }
    logger->info("P2: connected to P1.counter");

    using ObserverT = demo::ObserverNode<decltype(counter_ref)>;
    ObserverT observer(ObserverT::Inputs{logger, timers, counter_ref});

    // Mux is needed so CALL_REPLY frames coming back from P1 land in
    // our RemoteRef's reply demux. The observer itself has no inbound
    // remote messages, so we don't bind a listening socket for it —
    // just watch the outbound client fd for replies.
    demo::runtime::TipcMux mux;
    mux.watch_remote_ref(counter_ref);
    mux.start();

    observer.start();
    observer.kick_off();

    int run_ms = 5000;
    if (const char* env = std::getenv("DEMO_RUN_MS")) run_ms = std::atoi(env);
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(run_ms);
    while (!g_shutdown.load() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    logger->info("=== P2 stopping ===");
    mux.stop();
    observer.stop("normal");

    logger->info("=== P2 summary: polls=" +
                 std::to_string(observer.state().polls_issued) +
                 " replies_ok=" +
                 std::to_string(observer.state().replies_ok) +
                 " timeouts=" + std::to_string(observer.state().timeouts) +
                 " last_value=" + std::to_string(observer.state().last_value) +
                 " ===");
    return 0;
}

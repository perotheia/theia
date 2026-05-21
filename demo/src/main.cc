// gen_server demo: 3 nodes in one process.
//
//   CounterNode  holds int counter, serves Get/Inc + handle_info(tick)
//   DriverNode   casts Inc{5}×10, then call(Get) → asserts reply=50
//   TickerNode   posts handle_info("tick") to counter every 100ms ×10
//
// Each node has its own std::thread. Communication: in-process
// std::function mailboxes — no TIPC, no nanopb encode/decode. The
// nanopb-generated structs are just POD message types passed by value.

#include "GenServer.hh"
#include "Logger.hh"
#include "NodeRef.hh"
#include "TimerService.hh"
#include "counter_node.hh"
#include "demo_codecs.hh"        // RemoteCodec specializations (used by tracer)
#include "driver_node.hh"
#include "ticker_node.hh"

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    auto logger = platform::runtime::MakeConsoleLogger();
    logger->info("=== gen_server demo start ===");

    // Timer service: spawns a background thread; joined at ~end of scope.
    // Outlives the nodes that depend on it (declared earlier) so any
    // outstanding timers are guaranteed to see a live destination.
    demo::runtime::TimerService timers;

    demo::CounterNode counter(demo::CounterNodeInputs{logger});

    demo::TickerNode  ticker(
        demo::TickerNodeInputs{logger, timers, /*max_ticks=*/10});

    // Driver targets counter locally; both ports use the same ref.
    demo::runtime::LocalRef<demo::CounterNode> counter_ref(counter);
    using DriverT = demo::DriverNode<
        demo::runtime::LocalRef<demo::CounterNode>,
        demo::runtime::LocalRef<demo::CounterNode>>;
    DriverT driver(DriverT::Inputs{
        logger, timers, counter_ref, counter_ref});

    counter.start();
    driver.start();
    ticker.start();

    driver.kick_off();
    ticker.kick_off();

    // Wait for driver to finish its sequence. Cap at 5s so a deadlock
    // surfaces instead of hanging the test.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!driver.state().done.load()) {
        if (std::chrono::steady_clock::now() > deadline) {
            logger->error("driver did not finish within 5s — aborting");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Give the ticker time to fire all 10 ticks before tearing down.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    ticker.stop();
    driver.stop();
    counter.stop();

    const auto& s = driver.state();
    // 1 sync call + 3 async (send_request/wait_one) = 4 replies expected.
    // cancel_remaining_ms should be a positive value close to 2000 —
    // strict cancel caught the timer before it fired. Anything < 500
    // would mean the cancel ran too slowly; -1 would mean it had
    // already fired (also a failure for this test).
    bool ok = s.replies_ok == 4 &&
              s.last_value == s.expected_value &&
              s.timeouts == 0 &&
              s.errors == 0 &&
              s.cancel_remaining_ms > 1000;
    logger->info(std::string("=== summary: replies_ok=") +
                 std::to_string(s.replies_ok) +
                 " last_value=" + std::to_string(s.last_value) +
                 " expected=" + std::to_string(s.expected_value) +
                 " timeouts=" + std::to_string(s.timeouts) +
                 " errors=" + std::to_string(s.errors) +
                 " cancel_remaining=" + std::to_string(s.cancel_remaining_ms) +
                 "ms" + (ok ? " PASS" : " FAIL"));
    return ok ? 0 : 1;
}

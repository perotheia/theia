// CounterNode — holds an integer, serves Get (call) and Inc (cast).
//
// Demonstrates all three gen_server callbacks on the server side:
//   handle_call(Get)  → GetReply { state.counter }
//   handle_cast(Inc)  → state.counter += n
//   handle_info(tick) → log the current value
//
// State touched only by the owning thread (the GenServer base's thread).
// Handlers are noexcept-by-convention; exceptions in handle_call surface
// as a CallTag::Error on the caller side via the std::promise path.

#pragma once

#include "GenServer.hh"
#include "Logger.hh"

#include "demo/system/system.pb.h"

#include <cstdint>
#include <memory>

namespace demo {

struct CounterState {
    int32_t counter = 0;
    // Cleared on construction, set to non-empty string by terminate.
    // The test suite reads this through state() AFTER stop() returns
    // to confirm terminate ran exactly once with the right reason.
    std::string terminated_with;
};

// Per-node Inputs struct: every dependency the ctor needs. The
// composition's main builds this in scope and passes by const&.
// CounterNode has no outbound ports, so its Inputs is just the logger.
struct CounterNodeInputs {
    std::shared_ptr<platform::runtime::Logger> logger;
};

class CounterNode
    : public runtime::GenServer<CounterNode, CounterState> {
public:
    // Used by the tracer registry: the framework's dispatch lambdas
    // pull this when emitting trace events. Stays the same across
    // instances; one Tracer per node-type, not per-instance.
    static constexpr const char* kNodeName = "CounterNode";

    explicit CounterNode(const CounterNodeInputs& in);

    // gen_server callbacks. Names and signatures mirror OTP exactly,
    // modulo State& replacing Erlang's threaded state return.
    demo_system_GetReply handle_call(const demo_system_Get& req,
                                       CounterState& s);

    void handle_cast(const demo_system_Inc& msg,
                      CounterState& s);

    void handle_info(const char* info, CounterState& s);

    // Optional gen_server-style callback. Runs on the counter's own
    // thread after stop() drains the mailbox, before the loop exits.
    void terminate(const char* reason, CounterState& s) noexcept;

private:
    std::shared_ptr<platform::runtime::Logger> logger_;
};

}  // namespace demo

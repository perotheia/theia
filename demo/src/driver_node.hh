// DriverNode — the demo's driver. Casts Inc{5} ten times to the
// CounterNode, then issues call(Get) and asserts the reply matches.
//
// Templated on the ref types for its two outbound ports so the same
// source compiles for any composition: local-only (LocalRef) or
// cross-process (RemoteRef). The generator instantiates with the
// right types based on the composition's `on process` placement.
//
// Demonstrates the CALLER side of the gen_server API:
//   * cast(counter, Inc{5})                — async, no reply
//   * call_and_dispatch<GetReply>(this, counter, Get{}, act, 5000)
//                                          — sync, routes reply to
//                                            handle_call_result()

#pragma once

#include "GenServer.hh"
#include "Logger.hh"
#include "TimerService.hh"

#include "demo/system/system.pb.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace demo {

// Asynchronous-completion-token (ACT) — opaque to the framework,
// owned by the caller, echoed in CallResult. Carries a kind tag and a
// monotonic id so handle_call_result can route per-request.
struct DriverAct {
    enum class Kind { GetCounter };
    Kind     kind = Kind::GetCounter;
    uint32_t request_id = 0;
};

struct DriverState {
    int32_t  expected_value = 0;
    int32_t  last_value     = 0;
    uint32_t replies_ok     = 0;
    uint32_t timeouts       = 0;
    uint32_t errors         = 0;
    int      cancel_remaining_ms = -2;
    std::atomic<bool> done{false};
};

// Templated Inputs: each port-typed member's ref type is whatever the
// composition wires in (LocalRef<T> or RemoteRef<T, addr, inst>).
template <typename IncOutRef, typename CounterCallRef>
struct DriverNodeInputs {
    std::shared_ptr<platform::runtime::Logger> logger;
    runtime::TimerService&  timers;
    IncOutRef&              inc_out;        // sender port → IncIface
    CounterCallRef&         counter_call;   // client port → CounterSrv
};

template <typename IncOutRef, typename CounterCallRef>
class DriverNode
    : public runtime::GenServer<
                DriverNode<IncOutRef, CounterCallRef>, DriverState> {
public:
    static constexpr const char* kNodeName = "DriverNode";
    using Inputs = DriverNodeInputs<IncOutRef, CounterCallRef>;

    explicit DriverNode(const Inputs& in)
        : logger_(in.logger),
          timers_(in.timers),
          inc_out_(in.inc_out),
          counter_call_(in.counter_call) {}

    // Kick off the driver scenario on the driver's own thread.
    void kick_off();

    // gen_server callbacks.
    void handle_info(const char* info, DriverState& s);

    // Caller-side reply handlers.
    void handle_call_result(const demo_system_GetReply& reply,
                             const DriverAct& act,
                             DriverState& s);
    void handle_call_error(const std::string& reason,
                            const DriverAct& act,
                            DriverState& s);
    void handle_call_timeout(const DriverAct& act,
                              DriverState& s);

private:
    std::shared_ptr<platform::runtime::Logger> logger_;
    runtime::TimerService&  timers_;
    IncOutRef&              inc_out_;
    CounterCallRef&         counter_call_;
};

}  // namespace demo

// Out-of-line method definitions in a sibling include — keeps the
// header light and the implementation greppable.
#include "driver_node.tcc"

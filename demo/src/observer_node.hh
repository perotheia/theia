// ObserverNode — periodically polls counter's Get and logs the value.
// Templated on the ref type so the same source compiles whether
// counter is local (LocalRef) or remote (RemoteRef).

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

struct ObserverAct { uint32_t request_id = 0; };

struct ObserverState {
    uint32_t polls_issued  = 0;
    uint32_t replies_ok    = 0;
    uint32_t timeouts      = 0;
    int32_t  last_value    = -1;
};

template <typename CounterCallRef>
struct ObserverNodeInputs {
    std::shared_ptr<platform::runtime::Logger> logger;
    runtime::TimerService& timers;
    CounterCallRef&        counter_call;
};

template <typename CounterCallRef>
class ObserverNode
    : public runtime::GenServer<ObserverNode<CounterCallRef>, ObserverState> {
public:
    static constexpr const char* kNodeName = "ObserverNode";
    using Inputs = ObserverNodeInputs<CounterCallRef>;

    explicit ObserverNode(const Inputs& in)
        : logger_(in.logger),
          timers_(in.timers),
          counter_call_(in.counter_call) {}

    void kick_off();

    void handle_info(const char* info, ObserverState& s);

    void handle_call_result(const demo_system_GetReply& reply,
                              const ObserverAct& act,
                              ObserverState& s);
    void handle_call_timeout(const ObserverAct& act, ObserverState& s);
    void handle_call_error(const std::string& reason,
                            const ObserverAct& act,
                            ObserverState& s);

private:
    std::shared_ptr<platform::runtime::Logger> logger_;
    runtime::TimerService& timers_;
    CounterCallRef&        counter_call_;
    std::atomic<uint32_t>  next_req_id_{1};
};

}  // namespace demo

#include "observer_node.tcc"

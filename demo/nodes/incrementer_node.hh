// IncrementerNode — every 300ms casts Inc{2} to counter. Templated on
// the ref type so the same source compiles for local- and cross-process.

#pragma once

#include "GenServer.hh"
#include "Logger.hh"
#include "TimerService.hh"

#include "system/demo/demo.pb.h"

#include <cstdint>
#include <memory>

namespace demo {

struct IncrementerState {
    uint32_t casts_sent = 0;
};

template <typename IncOutRef>
struct IncrementerNodeInputs {
    std::shared_ptr<platform::runtime::Logger> logger;
    runtime::TimerService& timers;
    IncOutRef&             inc_out;
};

template <typename IncOutRef>
class IncrementerNode
    : public runtime::GenServer<IncrementerNode<IncOutRef>, IncrementerState> {
public:
    static constexpr const char* kNodeName = "IncrementerNode";
    using Inputs = IncrementerNodeInputs<IncOutRef>;

    explicit IncrementerNode(const Inputs& in)
        : logger_(in.logger), timers_(in.timers), inc_out_(in.inc_out) {}

    void kick_off();

    void handle_info(const char* info, IncrementerState& s);

private:
    std::shared_ptr<platform::runtime::Logger> logger_;
    runtime::TimerService& timers_;
    IncOutRef&             inc_out_;
};

}  // namespace demo

#include "incrementer_node.tcc"

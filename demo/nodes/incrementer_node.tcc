// IncrementerNode template method definitions.

#pragma once

#include "NodeRef.hh"
#include "TimerService.hh"

#include <cstring>

namespace demo {

template <typename IncOutRef>
void IncrementerNode<IncOutRef>::kick_off() {
    runtime::post_info(*this, "tick");
}

template <typename IncOutRef>
void IncrementerNode<IncOutRef>::handle_info(const char* info,
                                              IncrementerState& s) {
    if (std::strcmp(info, "tick") != 0) return;

    services_demo_Inc msg{};
    msg.n = 2;
    runtime::cast(inc_out_, msg);
    ++s.casts_sent;

    if ((s.casts_sent % 10) == 0) {
        logger_->info("[incrementer] casts_sent=" +
                      std::to_string(s.casts_sent));
    }
    runtime::send_after(timers_, 300, *this, "tick");
}

}  // namespace demo

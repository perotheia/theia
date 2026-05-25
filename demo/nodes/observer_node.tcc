// ObserverNode template method definitions.

#pragma once

#include "NodeRef.hh"
#include "TimerService.hh"

#include <cstring>

namespace demo {

template <typename CounterCallRef>
void ObserverNode<CounterCallRef>::kick_off() {
    runtime::post_info(*this, "poll");
}

template <typename CounterCallRef>
void ObserverNode<CounterCallRef>::handle_info(const char* info,
                                                 ObserverState& s) {
    if (std::strcmp(info, "poll") != 0) return;

    ObserverAct act{next_req_id_.fetch_add(1)};
    ++s.polls_issued;

    auto r = runtime::call<services_demo_GetReply>(
        counter_call_, services_demo_Get{}, act, /*timeout_ms=*/500);
    switch (r.tag) {
        case runtime::CallTag::Reply:
            handle_call_result(r.reply, r.act, s);
            break;
        case runtime::CallTag::Timeout:
            handle_call_timeout(r.act, s);
            break;
        case runtime::CallTag::Error:
            handle_call_error(r.error, r.act, s);
            break;
    }
    runtime::send_after(timers_, 200, *this, "poll");
}

template <typename CounterCallRef>
void ObserverNode<CounterCallRef>::handle_call_result(
    const services_demo_GetReply& reply,
    const ObserverAct& act,
    ObserverState& s) {
    s.last_value = reply.value;
    ++s.replies_ok;
    logger_->info("[observer] poll #" + std::to_string(act.request_id) +
                  " value=" + std::to_string(reply.value));
}

template <typename CounterCallRef>
void ObserverNode<CounterCallRef>::handle_call_timeout(
    const ObserverAct& act, ObserverState& s) {
    ++s.timeouts;
    logger_->error("[observer] timeout req_id=" +
                   std::to_string(act.request_id));
}

template <typename CounterCallRef>
void ObserverNode<CounterCallRef>::handle_call_error(
    const std::string& reason,
    const ObserverAct& act,
    ObserverState& s) {
    logger_->error("[observer] error req_id=" +
                   std::to_string(act.request_id) + ": " + reason);
    (void)s;
}

}  // namespace demo

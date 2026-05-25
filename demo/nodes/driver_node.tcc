// DriverNode template method definitions. Included from driver_node.hh
// so the compiler sees the bodies at every instantiation point.

#pragma once

#include "NodeRef.hh"
#include "TimerService.hh"

#include <chrono>
#include <cstring>
#include <thread>

namespace demo {

template <typename IncOutRef, typename CounterCallRef>
void DriverNode<IncOutRef, CounterCallRef>::kick_off() {
    runtime::post_info(*this, "run");
}

template <typename IncOutRef, typename CounterCallRef>
void DriverNode<IncOutRef, CounterCallRef>::handle_info(const char* info,
                                                         DriverState& s) {
    if (std::strcmp(info, "run") != 0) {
        logger_->info(std::string("[driver] handle_info(other) — ") + info);
        return;
    }

    logger_->info("[driver] starting: 10× cast(Inc{5}), then call(Get)");
    for (int i = 0; i < 10; ++i) {
        services_demo_Inc msg{};
        msg.n = 5;
        runtime::cast(inc_out_, msg);
    }
    s.expected_value = 50;

    // Tiny delay so casts settle on the counter thread before the
    // first Get goes out — see the in-process demo for the rationale.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    DriverAct act{DriverAct::Kind::GetCounter, /*request_id=*/1};
    services_demo_Get req{};
    runtime::call_and_dispatch<services_demo_GetReply>(
        *this, counter_call_, req, act, /*timeout_ms=*/2000);

    // Phase 2: 3× send_request, collect via wait_one. check_response
    // exercises the NoReply branch.
    logger_->info("[driver] phase 2: 3× send_request, collect via wait_one");
    runtime::RequestIdCollection<services_demo_GetReply, DriverAct> col;
    DriverAct early_act{DriverAct::Kind::GetCounter, /*request_id=*/2};
    auto rid_early = runtime::send_request<services_demo_GetReply>(
        counter_call_, services_demo_Get{}, early_act);
    {
        auto chk = runtime::check_response(rid_early);
        logger_->info(std::string("[driver] check_response(early) tag=") +
            (chk.tag == runtime::CheckTag::NoReply ? "NoReply" :
             chk.tag == runtime::CheckTag::Reply   ? "Reply" :
             chk.tag == runtime::CheckTag::Error   ? "Error" : "Timeout"));
        if (chk.tag == runtime::CheckTag::Reply) {
            handle_call_result(chk.reply, chk.act, s);
        } else {
            col.add(std::move(rid_early));
        }
    }
    for (uint32_t i = 3; i <= 4; ++i) {
        DriverAct a{DriverAct::Kind::GetCounter, /*request_id=*/i};
        col.add(runtime::send_request<services_demo_GetReply>(
            counter_call_, services_demo_Get{}, a));
    }
    while (!col.empty()) {
        auto r = col.wait_one(/*timeout_ms=*/2000);
        switch (r.tag) {
            case runtime::CallTag::Reply:
                handle_call_result(r.reply, r.act, s);
                break;
            case runtime::CallTag::Error:
                handle_call_error(r.error, r.act, s);
                break;
            case runtime::CallTag::Timeout:
                handle_call_timeout(r.act, s);
                break;
        }
    }

    // Phase 3: send_after + strict cancel_timer. Target is *this so we
    // don't need a typed ref to counter as a GenServerBase&. The
    // message is cancelled before fire so the destination's mailbox
    // never sees it anyway — this is a runtime-API smoke test, not a
    // wire-level test.
    logger_->info("[driver] phase 3: send_after + cancel_timer");
    auto tref = runtime::send_after(timers_, 2000, *this,
                                     "should_never_fire");
    int remaining = runtime::cancel_timer(timers_, std::move(tref));
    s.cancel_remaining_ms = remaining;
    logger_->info("[driver] cancel_timer remaining=" +
                  std::to_string(remaining) + "ms (expect close to 2000)");

    s.done.store(true);
}

template <typename IncOutRef, typename CounterCallRef>
void DriverNode<IncOutRef, CounterCallRef>::handle_call_result(
    const services_demo_GetReply& reply,
    const DriverAct& act,
    DriverState& s) {
    s.last_value = reply.value;
    ++s.replies_ok;
    logger_->info("[driver] handle_call_result(req_id=" +
                  std::to_string(act.request_id) +
                  ") value=" + std::to_string(reply.value) +
                  " expected=" + std::to_string(s.expected_value));
}

template <typename IncOutRef, typename CounterCallRef>
void DriverNode<IncOutRef, CounterCallRef>::handle_call_error(
    const std::string& reason,
    const DriverAct& act,
    DriverState& s) {
    ++s.errors;
    logger_->error("[driver] handle_call_error(req_id=" +
                   std::to_string(act.request_id) +
                   ") reason=" + reason);
}

template <typename IncOutRef, typename CounterCallRef>
void DriverNode<IncOutRef, CounterCallRef>::handle_call_timeout(
    const DriverAct& act,
    DriverState& s) {
    ++s.timeouts;
    logger_->error("[driver] handle_call_timeout(req_id=" +
                   std::to_string(act.request_id) + ")");
}

}  // namespace demo

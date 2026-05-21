// TestServer / TestCaller — minimal nodes used across the suite.
//
// TestServer is a versatile gen_server: a counter you can Inc/Get,
// a "DelayedAnswer" call that sleeps before replying (for timeout
// tests), a "Stop" cast that flips a flag, plus terminate(Reason)
// recording. Mirrors the shape of gen_server_SUITE's test server.
//
// TestCaller is the client-side fixture: it receives reply/timeout/
// error callbacks and records them so test cases can assert.
//
// Both intentionally simple; no protobuf, just plain C++ types.

#pragma once

#include "GenServer.hh"
#include "Logger.hh"
#include "TimerService.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace test {

// ---- Server messages (typed C++ structs, no nanopb) ---------------------

struct Inc       { int32_t n; };
struct Get       { };
struct GetReply  { int32_t value; };

// Sleep for `ms` ms before replying with `tag`. Used to exercise call
// timeouts (caller passes a tiny timeout vs. a long delay).
struct DelayedAnswer       { int ms; int32_t tag; };
struct DelayedAnswerReply  { int32_t tag; };

// ---- Server ------------------------------------------------------------

struct TestServerState {
    int32_t counter = 0;
    std::string terminated_with;   // populated by terminate()
    std::atomic<uint32_t> casts_received{0};
    std::atomic<uint32_t> infos_received{0};
    std::string last_info;
};

class TestServer
    : public demo::runtime::GenServer<TestServer, TestServerState> {
public:
    static constexpr const char* kNodeName = "TestServer";
    GetReply handle_call(const Get&, TestServerState& s) {
        return GetReply{s.counter};
    }
    DelayedAnswerReply handle_call(const DelayedAnswer& msg,
                                    TestServerState&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(msg.ms));
        return DelayedAnswerReply{msg.tag};
    }
    void handle_cast(const Inc& msg, TestServerState& s) {
        s.counter += msg.n;
        s.casts_received.fetch_add(1);
    }
    void handle_info(const char* info, TestServerState& s) {
        s.last_info = info;
        s.infos_received.fetch_add(1);
    }
    void terminate(const char* reason, TestServerState& s) noexcept {
        s.terminated_with = reason;
    }
};

// ---- Caller fixture ----------------------------------------------------

// ACT type used by send_request_with_timeout tests. Carries an id so
// handle_call_result/timeout can tell concurrent calls apart.
struct CallAct { uint32_t id; };

// State recorded by the caller's reply/timeout/error handlers.
struct CallerState {
    std::vector<int32_t>     replies;   // values of GetReply received
    std::vector<uint32_t>    timeouts;  // act.id values that timed out
    std::vector<std::string> errors;
    std::atomic<bool> done{false};
};

// Caller is a gen_server too — the auto-routed reply/timeout from
// send_request_with_timeout posts back onto its mailbox, which means
// the typed callbacks run on the caller's own thread.
class TestCaller
    : public demo::runtime::GenServer<TestCaller, CallerState> {
public:
    static constexpr const char* kNodeName = "TestCaller";
    void handle_call_result(const GetReply& r, const CallAct& a,
                             CallerState& s) {
        // Record the value; a.id is logged for trace context.
        (void)a;
        s.replies.push_back(r.value);
    }
    void handle_call_result(const DelayedAnswerReply& r, const CallAct& a,
                             CallerState& s) {
        (void)a;
        s.replies.push_back(r.tag);
    }
    void handle_call_timeout(const CallAct& a, CallerState& s) {
        s.timeouts.push_back(a.id);
    }
    void handle_call_error(const std::string& reason, const CallAct& a,
                            CallerState& s) {
        (void)a;
        s.errors.push_back(reason);
    }

    // Driving helper — exposes the caller's mailbox for test cases to
    // post a "go" signal that runs the actual scenario on this node's
    // own thread. The scenario invokes the framework APIs and sets
    // state.done when finished.
    void handle_info(const char* /*info*/, CallerState& /*s*/) {
        // Default no-op; per-case scenarios are kicked off from test
        // case code, not via handle_info.
    }
};

}  // namespace test

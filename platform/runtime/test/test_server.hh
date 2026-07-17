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

#include "platform_runtime_test/messages.pb.h"   // CFeed (codec-backed feed)

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

// A periodic STATE-LIKE feed update carrying a freshness `seq` (a corridor /
// pose / frame — only the newest matters). `handle_cast(Feed)` sleeps `work_ms`
// to model a consumer slower than the producer, and records EVERY seq it
// processes — so a test can show a plain FIFO mailbox drains all stale updates
// (the conflating-mailbox failure) vs. an opt-in conflating port that would keep
// only the latest. See docs/tasks genserver-conflating-mailbox.
struct Feed { uint32_t seq; int work_ms; };

// ---- Server ------------------------------------------------------------

struct TestServerState {
    int32_t counter = 0;
    std::string terminated_with;   // populated by terminate()
    std::atomic<uint32_t> casts_received{0};
    std::atomic<uint32_t> infos_received{0};
    std::string last_info;
    // Feed-conflation observability: the ordered seqs handle_cast(Feed) actually
    // processed, and the newest seq ever enqueued (stamped by the producer).
    std::vector<uint32_t> feed_seqs_handled;
    std::atomic<uint32_t> feed_seq_enqueued{0};
};

class TestServer
    : public theia::runtime::GenServer<TestServer, TestServerState> {
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
    // Slow state-like feed handler: record the seq, then sleep to model a
    // consumer slower than the producer. On a FIFO mailbox this drains every
    // stale seq in order (the failure); a conflating port would only ever see
    // the newest pending seq.
    void handle_cast(const Feed& msg, TestServerState& s) {
        s.feed_seqs_handled.push_back(msg.seq);
        if (msg.work_ms > 0)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(msg.work_ms));
    }
    // Codec-backed feed (has a service_id) — same observability as Feed, but a
    // type that can be marked [conflate] and routed through the local keep-latest
    // cast() path. Used by case_feed_conflation_local_cast.
    void handle_cast(const platform_runtime_test_CFeed& msg, TestServerState& s) {
        s.feed_seqs_handled.push_back(msg.seq);
        if (msg.work_ms > 0)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(msg.work_ms));
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
    : public theia::runtime::GenServer<TestCaller, CallerState> {
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

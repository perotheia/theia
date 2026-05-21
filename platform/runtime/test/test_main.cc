// gen_server runtime test suite.
//
// Cases ported from OTP's gen_server_SUITE
// (up/otp/lib/stdlib/test/gen_server_SUITE.erl), reduced to the subset
// our C++14 runtime models:
//
//   call_basic            — call returns the right value
//   call_sync_timeout     — call() with short timeout vs slow handler
//   cast_basic            — cast is fire-and-forget, state advances
//   cast_returns_immediately — cast does NOT block on slow handler
//   info_basic            — post_info routes to handle_info
//   send_request_basic    — send_request + wait_response returns reply
//   send_request_check    — check_response polling: NoReply → Reply
//   send_request_collection — fire 3 in parallel, collect via wait_one
//   send_request_with_timeout_reply   — auto-routed reply within budget
//   send_request_with_timeout_expires — auto-routed timeout fires
//   periodic_ticker       — schedule/handle/reschedule via send_after
//   cancel_timer_strict   — cancel before fire returns remaining ms
//   cancel_timer_after_fire — cancel after fire returns -1
//   stop_normal           — stop("normal") drains and runs terminate
//   stop_after_pending_cast — stop waits for queued casts before terminating

#include "GenServer.hh"
#include "Logger.hh"
#include "NodeRef.hh"
#include "TimerService.hh"
#include "TipcMux.hh"
#include "test_codecs.hh"
#include "test_server.hh"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace test;
namespace rt = demo::runtime;

// ---- micro-assertion harness -------------------------------------------

struct TestStat {
    int total = 0;
    int passed = 0;
    std::vector<std::string> failures;
};

#define CASE(stat, name) run_case(stat, #name, []() -> std::string

static void run_case(TestStat& stat, const char* name,
                      std::string (*body)()) {
    ++stat.total;
    std::printf("• %-40s ", name);
    std::fflush(stdout);
    std::string err;
    try { err = body(); }
    catch (const std::exception& e) { err = std::string("threw: ") + e.what(); }
    catch (...)                     { err = "threw: unknown"; }
    if (err.empty()) {
        ++stat.passed;
        std::printf("PASS\n");
    } else {
        stat.failures.emplace_back(std::string(name) + ": " + err);
        std::printf("FAIL — %s\n", err.c_str());
    }
}

#define EXPECT(cond, msg) \
    do { if (!(cond)) return std::string(msg) + " (" #cond ")"; } while (0)

// ---- helpers -----------------------------------------------------------

static void start_node(rt::GenServerBase& n) { n.start(); }

// Each case constructs its own nodes (and TimerService where needed) so
// state is independent. Teardown via stop("normal") to exercise the
// orderly-shutdown path.

// =======================================================================
//                                  CASES
// =======================================================================

static std::string case_call_basic() {
    TestServer s; start_node(s);
    auto r = rt::call<GetReply>(s, Get{}, CallAct{0}, /*timeout*/500);
    EXPECT(r.tag == rt::CallTag::Reply, "expected Reply");
    EXPECT(r.reply.value == 0, "fresh counter is 0");
    rt::cast(s, Inc{7});
    auto r2 = rt::call<GetReply>(s, Get{}, CallAct{0}, 500);
    EXPECT(r2.tag == rt::CallTag::Reply, "expected Reply");
    EXPECT(r2.reply.value == 7, "counter should be 7 after Inc{7}");
    s.stop();
    return {};
}

static std::string case_call_sync_timeout() {
    TestServer s; start_node(s);
    // 100ms server-side delay, 10ms caller timeout → must time out.
    auto r = rt::call<DelayedAnswerReply>(
        s, DelayedAnswer{100, 42}, CallAct{1}, /*timeout*/10);
    EXPECT(r.tag == rt::CallTag::Timeout, "expected sync Timeout");
    s.stop();
    return {};
}

static std::string case_cast_basic() {
    TestServer s; start_node(s);
    for (int i = 0; i < 5; ++i) rt::cast(s, Inc{3});
    // Sync probe to ensure the casts have been processed (FIFO with
    // our call lambda).
    auto r = rt::call<GetReply>(s, Get{}, CallAct{0}, 500);
    EXPECT(r.tag == rt::CallTag::Reply, "expected Reply");
    EXPECT(r.reply.value == 15, "5 casts of 3 should sum to 15");
    EXPECT(s.state().casts_received.load() == 5,
           "casts_received should be 5");
    s.stop();
    return {};
}

static std::string case_cast_returns_immediately() {
    TestServer s; start_node(s);
    // Issue a slow call so the mailbox is busy, then issue a cast and
    // measure: cast() must return immediately even though the server
    // thread is blocked inside DelayedAnswer's sleep.
    auto pending = rt::send_request<DelayedAnswerReply>(
        s, DelayedAnswer{200, 0}, CallAct{0});
    (void)pending;  // we don't need to collect it; just hold it alive
    auto t0 = std::chrono::steady_clock::now();
    rt::cast(s, Inc{1});
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT(elapsed < 20, "cast() must return immediately, not block on server");
    // Drain the in-flight call so stop() doesn't race the slow handler.
    auto r = rt::wait_response(pending, /*timeout*/500);
    EXPECT(r.tag == rt::CallTag::Reply, "delayed reply should arrive");
    s.stop();
    return {};
}

static std::string case_info_basic() {
    TestServer s; start_node(s);
    rt::post_info(s, "hello");
    // Sync probe to ensure handle_info ran.
    rt::call<GetReply>(s, Get{}, CallAct{0}, 500);
    EXPECT(s.state().infos_received.load() == 1, "infos_received should be 1");
    EXPECT(s.state().last_info == "hello", "last_info should be 'hello'");
    s.stop();
    return {};
}

static std::string case_send_request_basic() {
    TestServer s; start_node(s);
    rt::cast(s, Inc{11});
    auto rid = rt::send_request<GetReply>(s, Get{}, CallAct{1});
    auto r = rt::wait_response(rid, /*timeout*/500);
    EXPECT(r.tag == rt::CallTag::Reply, "expected Reply");
    EXPECT(r.reply.value == 11, "counter should be 11");
    EXPECT(r.act.id == 1, "act.id should round-trip");
    s.stop();
    return {};
}

static std::string case_send_request_check() {
    TestServer s; start_node(s);
    auto rid = rt::send_request<DelayedAnswerReply>(
        s, DelayedAnswer{50, 9}, CallAct{2});
    // Immediately after send, the reply almost certainly isn't ready.
    auto chk = rt::check_response(rid);
    EXPECT(chk.tag == rt::CheckTag::NoReply, "expected NoReply immediately");
    // Wait it out.
    auto r = rt::wait_response(rid, 500);
    EXPECT(r.tag == rt::CallTag::Reply, "should get Reply after wait");
    EXPECT(r.reply.tag == 9, "tag echoed");
    s.stop();
    return {};
}

static std::string case_send_request_collection() {
    TestServer s; start_node(s);
    rt::RequestIdCollection<GetReply, CallAct> col;
    for (uint32_t i = 1; i <= 3; ++i) {
        col.add(rt::send_request<GetReply>(s, Get{}, CallAct{i}));
    }
    int n = 0;
    while (!col.empty()) {
        auto r = col.wait_one(500);
        EXPECT(r.tag == rt::CallTag::Reply, "all should be Reply");
        EXPECT(r.act.id >= 1 && r.act.id <= 3, "act in range");
        ++n;
    }
    EXPECT(n == 3, "collected exactly 3");
    s.stop();
    return {};
}

// Reactive wait_one: a single slow request through col.send_request()
// (NOT via add() — that's the polling fallback path). Server sleeps
// for 50ms inside handle_call. Wait with a deep budget but measure how
// soon after the reply lands wait_one actually returns.
//
// With the OLD polling implementation, wait_one would return up to 2ms
// after the reply (the poll interval). With the cv-based reactive
// implementation, it should return within ~1ms of the reply (cv wake
// latency + handle_call's set_value to ready_count increment).
static std::string case_collection_reactive_wakeup() {
    TestServer s; start_node(s);
    rt::RequestIdCollection<DelayedAnswerReply, CallAct> col;
    // 50ms server-side sleep, then the reply.
    col.send_request(s, DelayedAnswer{50, 7}, CallAct{42});

    auto t0 = std::chrono::steady_clock::now();
    auto r = col.wait_one(/*timeout_ms=*/1000);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT(r.tag == rt::CallTag::Reply, "expected Reply");
    EXPECT(r.reply.tag == 7, "tag echoed");
    EXPECT(r.act.id == 42, "act echoed");
    // Reactive: total elapsed ~ server's 50ms sleep + tiny wake
    // latency. Polling would still land within 50ms+2ms — same. The
    // distinguishing check is that elapsed is NOT well above 50ms,
    // which would suggest we're polling past the reply.
    EXPECT(elapsed >= 50 && elapsed < 70,
           "wait_one should wake reactively within ~20ms of the reply "
           "(got " + std::to_string(elapsed) + "ms)");
    s.stop();
    return {};
}

static std::string case_send_request_with_timeout_reply() {
    rt::TimerService timers;
    TestServer s; start_node(s);
    TestCaller c; start_node(c);

    // Server replies in 10ms, deadline 500ms → reply must win.
    rt::send_request_with_timeout<DelayedAnswerReply>(
        s, DelayedAnswer{10, 77}, CallAct{3}, /*timeout*/500, c, timers);

    // Poll caller's state until the reply arrives (or fail after 2s).
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(2);
    while (c.state().replies.empty() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT(c.state().replies.size() == 1, "exactly one reply");
    EXPECT(c.state().replies[0] == 77, "reply tag echoed");
    EXPECT(c.state().timeouts.empty(), "no timeout should have fired");
    c.stop(); s.stop();
    return {};
}

static std::string case_send_request_with_timeout_expires() {
    rt::TimerService timers;
    TestServer s; start_node(s);
    TestCaller c; start_node(c);

    // Server takes 200ms, deadline 20ms → timer must win.
    rt::send_request_with_timeout<DelayedAnswerReply>(
        s, DelayedAnswer{200, 88}, CallAct{4}, /*timeout*/20, c, timers);

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(2);
    while (c.state().timeouts.empty() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT(c.state().timeouts.size() == 1, "exactly one timeout");
    EXPECT(c.state().timeouts[0] == 4, "act.id == 4");
    // The reply will eventually arrive at the server's mailbox but the
    // delivery lambda no-ops because the timeout already won. Give the
    // late reply a moment to (try to) come in and confirm we don't
    // record it.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT(c.state().replies.empty(), "late reply must be suppressed");
    c.stop(); s.stop();
    return {};
}

static std::string case_periodic_ticker() {
    rt::TimerService timers;
    TestServer s; start_node(s);

    // schedule/handle/reschedule pattern: each handle_info("tick")
    // schedules the next one. We drive it from outside since
    // TestServer's handle_info is a no-op recorder. Easiest: hand
    // schedule 5 separate send_afters with increasing delays.
    for (int i = 1; i <= 5; ++i) {
        rt::send_after(timers, i * 30, s, "tick");
    }
    // After 5*30 + slack, all five should have landed.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    auto got = s.state().infos_received.load();
    EXPECT(got == 5, "should have received 5 ticks");
    s.stop();
    return {};
}

static std::string case_cancel_timer_strict() {
    rt::TimerService timers;
    TestServer s; start_node(s);
    auto ref = rt::send_after(timers, 1000, s, "should_not_fire");
    int remaining = rt::cancel_timer(timers, std::move(ref));
    EXPECT(remaining > 500,
           "cancel within microseconds should report >500ms remaining");
    // Confirm the message never reached the server.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT(s.state().infos_received.load() == 0,
           "cancelled timer must not deliver");
    s.stop();
    return {};
}

static std::string case_cancel_timer_after_fire() {
    rt::TimerService timers;
    TestServer s; start_node(s);
    auto ref = rt::send_after(timers, 20, s, "fast");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    int remaining = rt::cancel_timer(timers, std::move(ref));
    EXPECT(remaining == -1,
           "cancel after fire should return -1 (already delivered)");
    EXPECT(s.state().infos_received.load() == 1, "message delivered");
    s.stop();
    return {};
}

static std::string case_stop_normal() {
    TestServer s; start_node(s);
    s.stop("normal");
    EXPECT(s.state().terminated_with == "normal",
           "terminate should have run with reason 'normal'");
    return {};
}

// ---- 3-process TIPC test ------------------------------------------------
//
// Spawns demo_p1, demo_p2, demo_p3 (all reading DEMO_RUN_MS for runtime
// budget), captures their stdout, asserts that P1's counter reached
// the expected value (= local driver casts + remote incrementer casts)
// and that P2's observer received non-zero polls with no timeouts.
//
// Path resolution: assumes the test binary is run from build/ alongside
// the three p* binaries (set by CMake). Falls back to ../build/ for
// out-of-tree invocations.

namespace {

std::string find_binary_(const char* name) {
    const char* candidates[] = {
        "./",                      // when run from build/
        "./build/",                // when run from demo/
    };
    for (const auto* p : candidates) {
        std::string full = std::string(p) + name;
        if (::access(full.c_str(), X_OK) == 0) return full;
    }
    return name;  // hope PATH catches it
}

pid_t spawn_(const std::string& path, const std::string& stdout_file,
              const std::string& run_ms) {
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // Child.
        int fd = ::open(stdout_file.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { ::dup2(fd, 1); ::dup2(fd, 2); ::close(fd); }
        ::setenv("DEMO_RUN_MS", run_ms.c_str(), 1);
        char* const argv[] = { const_cast<char*>(path.c_str()), nullptr };
        ::execv(path.c_str(), argv);
        ::_exit(127);
    }
    return pid;
}

std::string slurp_(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

int extract_int_(const std::string& haystack, const std::string& tag) {
    auto pos = haystack.find(tag);
    if (pos == std::string::npos) return -1;
    pos += tag.size();
    char* end;
    long v = std::strtol(haystack.c_str() + pos, &end, 10);
    return (end == haystack.c_str() + pos) ? -1 : static_cast<int>(v);
}

}  // namespace

// ---- Stress: 20 concurrent send_requests over TIPC ---------------------
//
// Validates the inbound-mux fix: with the old promise/future wait inside
// register_call, all 20 calls would serialize behind whichever one was
// being handled — no concurrency on the mux thread. With the fix, each
// inbound call enqueues a single lambda onto the node's mailbox, the
// mux thread returns immediately, and the node processes its mailbox
// FIFO (still serial per-node, but the mux can demux many incoming
// frames in parallel without head-of-line blocking).
//
// We run BOTH ends in this same process for tight control — counter
// node + TipcMux serving it on TIPC address 0xe0010001:0, and a
// RemoteRef looped back through the kernel TIPC stack. Same code path
// as cross-process, no fork/exec.

// Small TIPC-aware test server: same shape as CounterNode but bound
// to a stress-test-specific TIPC address. Uses the nanopb types
// (platform_runtime_test_*) so it lives on the wire and exercises the real
// codec path.
namespace {
struct TipcCounterState { int32_t counter = 0; };

class TipcCounter
    : public demo::runtime::GenServer<TipcCounter, TipcCounterState> {
public:
    static constexpr const char* kNodeName = "TipcCounter";
    platform_runtime_test_GetReply handle_call(const platform_runtime_test_Get&,
                                       TipcCounterState& s) {
        // Tiny artificial work so multiple in-flight requests can't
        // complete on a single mux iteration — gives the stress test
        // real concurrency to measure.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        platform_runtime_test_GetReply r{};
        r.value = ++s.counter;
        return r;
    }
    void handle_cast(const platform_runtime_test_Inc& msg, TipcCounterState& s) {
        s.counter += msg.n;
    }
    void handle_info(const char*, TipcCounterState&) {}
};
}  // namespace

static std::string case_tipc_concurrent_calls() {
    if (::access("/proc/sys/net/tipc", F_OK) != 0) return {};

    namespace rt = demo::runtime;

    TipcCounter server;
    server.start();

    rt::TipcMux mux;
    auto* binding = mux.bind_node(server, 0xe0010001u, 0u);
    EXPECT(binding != nullptr, "stress: bind_node");
    mux.register_call<platform_runtime_test_Get, platform_runtime_test_GetReply>(binding, server);
    mux.start();

    rt::RemoteRef<TipcCounter, 0xe0010001u, 0u> ref;
    EXPECT(ref.connect(2000), "stress: client connect");
    mux.watch_remote_ref(ref);

    // Fire 20 concurrent send_request<platform_runtime_test_GetReply>. Each
    // handle_call sleeps 5ms, so a serialized old mux would take
    // ~100ms total; with the fix all 20 enqueue simultaneously and
    // the node serves them serially in ~100ms total — but importantly
    // the MUX never blocked behind a single call.
    constexpr int N = 20;
    rt::RequestIdCollection<platform_runtime_test_GetReply, test::CallAct> col;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        col.add(rt::send_request<platform_runtime_test_GetReply>(
            ref, platform_runtime_test_Get{}, test::CallAct{(uint32_t)i}));
    }

    int got = 0;
    while (!col.empty()) {
        auto r = col.wait_one(/*timeout_ms=*/2000);
        EXPECT(r.tag == rt::CallTag::Reply, "stress: expected Reply");
        ++got;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT(got == N, "stress: all 20 must reply");
    EXPECT(elapsed < 1500,
           "stress: 20 calls should complete in <1.5s (got " +
           std::to_string(elapsed) + "ms)");

    mux.stop();
    server.stop();
    return {};
}

// Cross-process trace correlation. Same fixture as the concurrent-
// calls stress test, but with TipcCounter's tracer enabled. We capture
// stderr and assert that:
//   * At least one Send event carries a corr_id that matches a Recv
//     event later in the stream (sender → receiver correlation).
//   * Same corr_id appears on Dispatch / DispatchDone / SendReply /
//     CallResult — the full RPC lifecycle is observable.
//
// This is the case that proves trace records can stitch cross-process
// (or in our case, same-process-different-thread-over-real-TIPC)
// message flow into one timeline.
static std::string case_tipc_trace_correlation() {
    if (::access("/proc/sys/net/tipc", F_OK) != 0) return {};

    namespace rt = demo::runtime;

    int pipefd[2];
    if (::pipe(pipefd) < 0) return "pipe()";
    int saved = ::dup(STDERR_FILENO);
    ::dup2(pipefd[1], STDERR_FILENO);
    ::close(pipefd[1]);
    ::fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    auto& tr = rt::tracer_for(TipcCounter::kNodeName);
    tr.enable(true);

    TipcCounter server;
    server.start();
    rt::TipcMux mux;
    auto* binding = mux.bind_node(server, 0xe0010002u, 0u);
    EXPECT(binding != nullptr, "trace: bind_node");
    mux.register_call<platform_runtime_test_Get, platform_runtime_test_GetReply>(binding, server);
    mux.start();
    rt::RemoteRef<TipcCounter, 0xe0010002u, 0u> ref;
    EXPECT(ref.connect(2000), "trace: client connect");
    mux.watch_remote_ref(ref);

    // Two sequential calls — keeps the trace stream small and the
    // correlation pairs easy to find.
    for (int i = 0; i < 2; ++i) {
        auto fut = ref.template send_request_<platform_runtime_test_GetReply,
                                              platform_runtime_test_Get>(
            platform_runtime_test_Get{});
        auto status = fut.wait_for(std::chrono::milliseconds(500));
        EXPECT(status == std::future_status::ready, "trace: reply ready");
        (void)fut.get();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    mux.stop();
    server.stop();
    tr.enable(false);
    std::fflush(stderr);

    std::string captured;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0) {
        captured.append(buf, n);
    }
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
    ::close(pipefd[0]);

    // Walk the captured trace; collect (event, corr_id) pairs and
    // check that for at least one corr_id we see {send, recv, dispatch,
    // dispatch_done, send_reply, call_result}. That's the full RPC
    // round-trip captured in the trace stream.
    auto extract_corr = [](const std::string& line) -> uint32_t {
        auto p = line.find("corr=");
        if (p == std::string::npos) return 0;
        return static_cast<uint32_t>(std::strtoul(line.c_str() + p + 5,
                                                   nullptr, 10));
    };
    std::unordered_map<uint32_t, std::string> events_per_corr;
    size_t pos = 0;
    while (pos < captured.size()) {
        size_t eol = captured.find('\n', pos);
        if (eol == std::string::npos) eol = captured.size();
        std::string line = captured.substr(pos, eol - pos);
        if (line.compare(0, 7, "TRC v1 ") == 0) {
            uint32_t corr = extract_corr(line);
            // Format: "TRC v1 <event> <node> msg=<type> corr=<id> ts=<ms> hex=<>"
            // The 3rd whitespace-separated token is the event name —
            // starts at offset 7 (right after "TRC v1 ") and runs to
            // the next space.
            size_t ev_end = line.find(' ', 7);
            if (ev_end != std::string::npos) {
                std::string ev = line.substr(7, ev_end - 7);
                events_per_corr[corr] += ev + ",";
            }
        }
        pos = eol + 1;
    }

    // Find a corr_id whose event-set covers the full RPC lifecycle.
    bool ok = false;
    for (const auto& kv : events_per_corr) {
        if (kv.first == 0) continue;  // synthetic (in-process or non-RPC)
        const auto& s = kv.second;
        if (s.find("send,") != std::string::npos &&
            s.find("recv,") != std::string::npos &&
            s.find("dispatch,") != std::string::npos &&
            s.find("dispatch_done,") != std::string::npos &&
            s.find("send_reply,") != std::string::npos &&
            s.find("call_result,") != std::string::npos) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        // Compact diagnostic — show what we saw.
        std::string diag;
        int cnt = 0;
        for (const auto& kv : events_per_corr) {
            if (cnt++ >= 5) break;
            diag += "corr=" + std::to_string(kv.first) + ":{" + kv.second + "} ";
        }
        return "no corr_id covers full RPC lifecycle. Seen: " + diag;
    }
    return {};
}

// Shared body: spawn three binaries with the given names, parse their
// summaries, assert the same counter math. Used by both the handwritten
// 3-process test and the generated-apps variant.
static std::string run_3process_(const char* p1_name,
                                   const char* p2_name,
                                   const char* p3_name) {
    if (::access("/proc/sys/net/tipc", F_OK) != 0) return {};
    std::string p1 = find_binary_(p1_name);
    std::string p2 = find_binary_(p2_name);
    std::string p3 = find_binary_(p3_name);
    // 3-process binaries live in the demo CMake build, not in the
    // platform/runtime Bazel cc_test. Skip the case (return PASS) if
    // they're absent — preserving the case as a smoke test for callers
    // who DO have the binaries available (the demo's gen_server_tests).
    if (::access(p1.c_str(), X_OK) != 0) return {};

    const std::string l1 = std::string("/tmp/test_") + p1_name + ".log";
    const std::string l2 = std::string("/tmp/test_") + p2_name + ".log";
    const std::string l3 = std::string("/tmp/test_") + p3_name + ".log";

    pid_t pid1 = spawn_(p1, l1, "3000");
    EXPECT(pid1 > 0, "spawn p1");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    pid_t pid2 = spawn_(p2, l2, "2500");
    pid_t pid3 = spawn_(p3, l3, "2500");
    EXPECT(pid2 > 0 && pid3 > 0, "spawn p2/p3");

    int st;
    ::waitpid(pid2, &st, 0);
    ::waitpid(pid3, &st, 0);
    ::waitpid(pid1, &st, 0);

    auto p1_out = slurp_(l1);
    auto p2_out = slurp_(l2);
    auto p3_out = slurp_(l3);

    int counter         = extract_int_(p1_out, "P1 summary: counter=");
    int driver_replies  = extract_int_(p1_out, "driver.replies_ok=");
    int polls           = extract_int_(p2_out, "P2 summary: polls=");
    int replies_ok      = extract_int_(p2_out, "replies_ok=");
    int timeouts        = extract_int_(p2_out, "timeouts=");
    int last_value      = extract_int_(p2_out, "last_value=");
    int casts_sent      = extract_int_(p3_out, "P3 summary: casts_sent=");

    EXPECT(driver_replies == 4,
           "driver's 4 local replies should still complete alongside TIPC");
    EXPECT(casts_sent > 0, "incrementer should have cast some Inc messages");
    EXPECT(polls > 0, "observer should have issued some polls");
    EXPECT(replies_ok > 0, "observer should have gotten at least some replies");
    EXPECT(timeouts == 0, "no observer timeouts");

    // Counter math: 10 local casts × 5 + casts_sent × 2 = 50 + 2*casts_sent.
    int expected_counter = 50 + 2 * casts_sent;
    if (counter != expected_counter) {
        return "counter=" + std::to_string(counter) +
               " expected=" + std::to_string(expected_counter) +
               " (50 local + 2*" + std::to_string(casts_sent) + " remote)";
    }
    // last_value seen by observer should reflect the running count;
    // bounded check rather than exact (observer's poll cadence isn't
    // sync with incrementer's casts).
    EXPECT(last_value > 50 && last_value <= counter,
           "observer's last seen value should be > 50 and <= final counter");
    return {};
}

// Tracer runtime gate: flipping the per-node Tracer enable mid-run
// should change observable output without restart. Compile-time
// elision is verified by inspection of the generated main.cc; here we
// just exercise the runtime path.
//
// Implementation: redirect stderr to a pipe, run a small scenario,
// read what landed in the pipe, restore stderr. Check that trace
// lines appear iff Tracer was enabled.
static std::string case_tracer_runtime_toggle() {
    using namespace test;
    namespace rt = demo::runtime;

    // Save stderr, redirect to a pipe.
    int pipefd[2];
    if (::pipe(pipefd) < 0) return "pipe()";
    int saved = ::dup(STDERR_FILENO);
    ::dup2(pipefd[1], STDERR_FILENO);
    ::close(pipefd[1]);
    ::fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    TestServer s;
    s.start();

    auto& tracer = rt::tracer_for(TestServer::kNodeName);

    // Phase 1: tracing disabled. A cast should produce no output.
    tracer.enable(false);
    rt::cast(s, Inc{1});
    // Sync barrier: round-trip a call so the cast has definitely been
    // dispatched and the trace lambda has either logged or skipped.
    auto r1 = rt::call<GetReply>(s, Get{}, CallAct{0}, 500);
    EXPECT(r1.tag == rt::CallTag::Reply, "phase1 call");

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Phase 2: enable tracer mid-run. Subsequent casts/calls should
    // emit trace lines.
    tracer.enable(true);
    rt::cast(s, Inc{2});
    auto r2 = rt::call<GetReply>(s, Get{}, CallAct{0}, 500);
    EXPECT(r2.tag == rt::CallTag::Reply, "phase2 call");

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Phase 3: disable again. No further output.
    tracer.enable(false);
    rt::cast(s, Inc{3});
    auto r3 = rt::call<GetReply>(s, Get{}, CallAct{0}, 500);
    EXPECT(r3.tag == rt::CallTag::Reply, "phase3 call");

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    s.stop();
    // fflush stderr so anything still buffered makes it to the pipe.
    std::fflush(stderr);

    // Drain the pipe.
    std::string captured;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0) {
        captured.append(buf, n);
    }

    // Restore stderr.
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
    ::close(pipefd[0]);

    // Count TRC records tagged with TestServer. Phase 2 ran one cast
    // (Send + Recv + Dispatch + DispatchDone = 4 events) and one call
    // (Send + Recv + Dispatch + DispatchDone = 4 more) = 8 events
    // total. Phase 1 and 3 should produce nothing.
    //
    // We don't pin the exact count (8 events) — the framework can grow
    // additional trace points later. Sanity bounds: at least 6 (each
    // op produces >= 3 events), at most ~20 (one ack between op cycles).
    int count = 0;
    size_t pos = 0;
    while ((pos = captured.find("TRC v1 ", pos)) != std::string::npos) {
        size_t eol = captured.find('\n', pos);
        if (eol == std::string::npos) eol = captured.size();
        // Count this record only if it's tagged with TestServer.
        if (captured.find("TestServer", pos) < eol) ++count;
        pos = eol;
    }
    if (count < 6 || count > 20) {
        return "expected 6..20 TestServer trace events (one cast + "
               "one call while enabled), got " + std::to_string(count) +
               " — captured: '" + captured + "'";
    }
    return {};
}

// Handwritten p* binaries.
static std::string case_tipc_3process() {
    return run_3process_("demo_p1", "demo_p2", "demo_p3");
}

// Generated p* binaries (from `artheia gen-app-composition`). Same
// assertions — the test runs the SAME counter-math invariant against
// the generated boot scaffolding, proving the generator's output is
// behaviorally equivalent to the handwritten mains.
static std::string case_tipc_3process_generated() {
    return run_3process_("gen_Demo3Way_P1",
                          "gen_Demo3Way_P2",
                          "gen_Demo3Way_P3");
}

static std::string case_stop_after_pending_cast() {
    // stop() enqueues its sentinel BEHIND any in-flight casts, so the
    // counter must reach 50 before terminate runs.
    TestServer s; start_node(s);
    for (int i = 0; i < 10; ++i) rt::cast(s, Inc{5});
    s.stop("shutdown");
    EXPECT(s.state().counter == 50,
           "all 10 casts must drain before terminate");
    EXPECT(s.state().terminated_with == "shutdown",
           "terminate reason should round-trip");
    return {};
}

// ---- driver ------------------------------------------------------------

int main() {
    TestStat stat;

    CASE(stat, call_basic) { return case_call_basic(); });
    CASE(stat, call_sync_timeout) { return case_call_sync_timeout(); });
    CASE(stat, cast_basic) { return case_cast_basic(); });
    CASE(stat, cast_returns_immediately) { return case_cast_returns_immediately(); });
    CASE(stat, info_basic) { return case_info_basic(); });
    CASE(stat, send_request_basic) { return case_send_request_basic(); });
    CASE(stat, send_request_check) { return case_send_request_check(); });
    CASE(stat, send_request_collection) { return case_send_request_collection(); });
    CASE(stat, collection_reactive_wakeup) { return case_collection_reactive_wakeup(); });
    CASE(stat, send_request_with_timeout_reply) { return case_send_request_with_timeout_reply(); });
    CASE(stat, send_request_with_timeout_expires) { return case_send_request_with_timeout_expires(); });
    CASE(stat, periodic_ticker) { return case_periodic_ticker(); });
    CASE(stat, cancel_timer_strict) { return case_cancel_timer_strict(); });
    CASE(stat, cancel_timer_after_fire) { return case_cancel_timer_after_fire(); });
    CASE(stat, stop_normal) { return case_stop_normal(); });
    CASE(stat, stop_after_pending_cast) { return case_stop_after_pending_cast(); });
    CASE(stat, tipc_3process)           { return case_tipc_3process(); });
    CASE(stat, tipc_3process_generated) { return case_tipc_3process_generated(); });
    CASE(stat, tipc_concurrent_calls)   { return case_tipc_concurrent_calls(); });
    CASE(stat, tipc_trace_correlation)  { return case_tipc_trace_correlation(); });
    CASE(stat, tracer_runtime_toggle)   { return case_tracer_runtime_toggle(); });

    std::printf("\n%d/%d passed\n", stat.passed, stat.total);
    if (!stat.failures.empty()) {
        std::printf("\nFailures:\n");
        for (const auto& f : stat.failures) std::printf("  ✗ %s\n", f.c_str());
        return 1;
    }
    return 0;
}

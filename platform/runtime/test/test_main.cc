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
#include "GenStateM.hh"
#include "Logger.hh"
#include "NodeRef.hh"
#include "TimerService.hh"
#include "TipcMux.hh"
#include "test_codecs.hh"
#include "test_server.hh"
#include "test_statem.hh"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace test;
namespace rt = theia::runtime;

// ---- trace capture: decode the proto3 TraceRecord the producer submits --
//
// As of the egress rework (#400) the Tracer no longer writes "TRC v1 …"
// to stderr — it frames a proto3 TraceRecord and submits it over TIPC
// (lossy SOCK_DGRAM) to the log[trace] collector. For unit tests we use
// TraceSubmitter's test-sink seam to capture the encoded records in
// process, then decode the handful of fields we assert on. No AF_TIPC
// needed, deterministic.
struct CapturedRecord {
    std::string node_name;   // field 1 (src)
    std::string dst;         // field 2
    std::string msg_type;    // field 3
    uint32_t    corr_id = 0; // field 4 (msg_id)
    uint64_t    ts_ns   = 0; // field 5
    uint32_t    kind    = 0; // field 6 (TraceKind)
    std::string payload;     // field 7
};

// Minimal proto3 reader — varint + length-delimited, the only wire types
// TraceRecord uses. Mirrors the hand-encoder in Tracer.hh.
static CapturedRecord decode_trace_record(const std::string& w) {
    CapturedRecord r;
    size_t i = 0;
    auto rd_varint = [&](uint64_t& out) -> bool {
        out = 0; int shift = 0;
        while (i < w.size()) {
            uint8_t b = static_cast<uint8_t>(w[i++]);
            out |= (uint64_t)(b & 0x7f) << shift;
            if (!(b & 0x80)) return true;
            shift += 7;
        }
        return false;
    };
    while (i < w.size()) {
        uint64_t tag;
        if (!rd_varint(tag)) break;
        uint32_t field = tag >> 3, wire = tag & 7;
        if (wire == 0) {                 // varint field
            uint64_t v;
            if (!rd_varint(v)) break;
            if (field == 4) r.corr_id = (uint32_t)v;
            else if (field == 5) r.ts_ns = v;
            else if (field == 6) r.kind = (uint32_t)v;
        } else if (wire == 2) {          // length-delimited
            uint64_t len;
            if (!rd_varint(len) || i + len > w.size()) break;
            std::string s = w.substr(i, len); i += len;
            if (field == 1) r.node_name = s;
            else if (field == 2) r.dst = s;
            else if (field == 3) r.msg_type = s;
            else if (field == 7) r.payload = s;
        } else break;                    // unexpected wire type
    }
    return r;
}

// RAII trace capture: installs a TraceSubmitter test sink that collects
// decoded records; restores (clears) the sink on scope exit. The sink is
// mutex-guarded inside TraceSubmitter, but we also guard our vector since
// emit() can fire from node threads.
class TraceCapture {
public:
    TraceCapture() {
        rt::TraceSubmitter::instance().set_test_sink(
            [this](const std::string& wire) {
                std::lock_guard<std::mutex> lk(mu_);
                recs_.push_back(decode_trace_record(wire));
            });
    }
    ~TraceCapture() {
        rt::TraceSubmitter::instance().set_test_sink(nullptr);
    }
    std::vector<CapturedRecord> records() {
        std::lock_guard<std::mutex> lk(mu_);
        return recs_;
    }
private:
    std::mutex mu_;
    std::vector<CapturedRecord> recs_;
};

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

// REPRODUCES the conflating-mailbox failure (docs/tasks
// genserver-conflating-mailbox): a periodic STATE-LIKE feed into a consumer
// slower than the producer. The mailbox is a faithful-OTP unbounded FIFO
// (std::deque, enqueue=push_back), so it queues EVERY stale update and the
// consumer processes them all in order — decisions lag reality by the whole
// backlog. For a corridor/pose/frame only the NEWEST matters; the backlog is
// pure harm. This test DOCUMENTS the current (buggy) behavior so a future
// conflating-port fix can flip the assertion to "only the latest is handled".
static std::string case_feed_conflation_backlog() {
    TestServer s; start_node(s);
    // Burst 20 feed updates, each needing ~15ms of work → the producer outruns
    // the consumer and the mailbox backs up. seqs 1..20; newest is 20.
    const uint32_t N = 20;
    for (uint32_t i = 1; i <= N; ++i) {
        s.state().feed_seq_enqueued.store(i);
        rt::cast(s, Feed{i, /*work_ms*/15});
    }
    // Drain: a sync call sits BEHIND the whole feed backlog in the FIFO, so when
    // it returns every queued Feed has been handled.
    rt::call<GetReply>(s, Get{}, CallAct{0}, /*timeout*/5000);
    const auto& handled = s.state().feed_seqs_handled;

    // CURRENT (FIFO) BEHAVIOR — the bug: EVERY stale seq was processed, in order.
    EXPECT(handled.size() == N,
           "FIFO mailbox drains ALL stale feed updates (the conflation bug)");
    for (uint32_t i = 0; i < N; ++i)
        EXPECT(handled[i] == i + 1, "FIFO processes stale updates in order");
    // The consumer wasted work on 19 stale corridors before reaching the newest.
    EXPECT(handled.front() == 1 && handled.back() == N,
           "consumer processed oldest→newest, lagging reality by the backlog");

    // What a CONFLATING port SHOULD yield (the fix, once implemented): the
    // handler sees only the newest pending seq while it was busy — handled.size()
    // would be small (≈2: the first, then the coalesced latest), and
    // handled.back() == feed_seq_enqueued. Flip these when `[conflate]` lands.
    //   EXPECT(handled.size() < N, "conflating port drops stale updates");
    //   EXPECT(handled.back() == s.state().feed_seq_enqueued.load(), "newest wins");
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
        "./build/",                // when run from a consuming ws
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
    : public theia::runtime::GenServer<TipcCounter, TipcCounterState> {
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

    namespace rt = theia::runtime;

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

// Per-call RemoteRef create/destroy CHURN — the regression for the TipcMux
// reply-fd use-after-free. Each iteration builds a FRESH RemoteRef, connects
// (which watch_reply_fd's its socket on the mux), does one call, and lets the
// ref DESTRUCT (which unwatch_reply_fd's + closes the socket) — all while the
// mux's epoll loop is dispatching replies. The bug: the loop copied the sink
// (capturing the ref) out under the lock, RELEASED the lock, then invoked it —
// so ~RemoteRef on this thread could free the ref between the copy and the call
// → SIGSEGV in the loop thread under churn. Many threads × many iterations
// makes the window hittable (and TSan flags the race directly). The fix holds
// the mux lock across the whole reply find→recv→dispatch.
static std::string case_tipc_remoteref_churn() {
    if (::access("/proc/sys/net/tipc", F_OK) != 0) return {};
    namespace rt = theia::runtime;

    TipcCounter server;
    server.start();
    rt::TipcMux mux;
    auto* binding = mux.bind_node(server, 0xe0010003u, 0u);
    EXPECT(binding != nullptr, "churn: bind_node");
    mux.register_call<platform_runtime_test_Get,
                      platform_runtime_test_GetReply>(binding, server);
    rt::set_process_mux(&mux);   // so each ad-hoc ref's reply fd is pumped
    mux.start();

    constexpr int THREADS = 6;
    constexpr int ITERS = 40;
    std::atomic<int> ok{0}, fail{0};
    std::vector<std::thread> ths;
    for (int t = 0; t < THREADS; ++t) {
        ths.emplace_back([&] {
            for (int i = 0; i < ITERS; ++i) {
                rt::RemoteRef<TipcCounter, 0xe0010003u, 0u> ref;
                if (!ref.connect(1000)) { ++fail; continue; }
                auto rid = rt::send_request<platform_runtime_test_GetReply>(
                    ref, platform_runtime_test_Get{},
                    test::CallAct{(uint32_t)i});
                auto r = rt::wait_response(rid, /*timeout_ms=*/1500);
                if (r.tag == rt::CallTag::Reply) ++ok; else ++fail;
                // ref destructs HERE — unwatch_reply_fd + close, racing the loop.
            }
        });
    }
    for (auto& th : ths) th.join();

    mux.stop();
    rt::set_process_mux(nullptr);
    server.stop();
    // The contract is "no crash / no UAF under churn"; we also assert the bulk
    // of calls completed (a few connect failures under heavy churn are ok).
    EXPECT(ok.load() > (THREADS * ITERS) / 2,
           "churn: majority of calls must complete (got " +
           std::to_string(ok.load()) + "/" +
           std::to_string(THREADS * ITERS) + ")");
    return {};
}

// NESTED cross-FC RemoteRef call: a server whose handle_call ITSELF makes a
// blocking RemoteRef call to a SECOND server over TIPC. This is the diag-FC
// pattern (UdsRouter's handle_call → crypto_link/phm_link RemoteRef → crypto/phm)
// — a gen_server reaching another FC synchronously from inside a handler, each
// peer link with its OWN TipcMux reply pump. We prove: (1) it works (the front
// server returns the back server's value), and (2) the front node thread isn't
// deadlocked by calling out while serving a call (the back link's pump is a
// separate mux, so the reply lands even though the front node thread is blocked
// in handle_call).
namespace {

// The BACK server (the "crypto/phm" analogue): a plain TIPC counter at 0xe0010005.
// (Reuses TipcCounter's shape but a distinct address so both can run.)
struct BackState { int32_t v = 100; };
class BackServer : public theia::runtime::GenServer<BackServer, BackState> {
public:
    static constexpr const char* kNodeName = "BackServer";
    platform_runtime_test_GetReply handle_call(const platform_runtime_test_Get&,
                                               BackState& s) {
        platform_runtime_test_GetReply r{};
        r.value = ++s.v;        // 101, 102, …
        return r;
    }
    void handle_cast(const platform_runtime_test_Inc&, BackState&) {}
    void handle_info(const char*, BackState&) {}
};

// The FRONT server (the "UdsRouter" analogue): its handle_call reaches into the
// BACK server via a process-global RemoteRef + its own pump (lazily connected),
// exactly like crypto_link/phm_link. Returns whatever the back server replied.
struct FrontState { int32_t last = 0; };
struct BackLink {
    theia::runtime::TipcMux mux;
    theia::runtime::RemoteRef<BackServer, 0xe0010005u, 0u> ref;
    bool up = false;
    static BackLink& instance() { static BackLink l; return l; }
    bool ensure() {
        if (up) return true;
        if (!ref.connect(1500)) return false;
        mux.watch_remote_ref(ref);
        mux.start();
        up = true;
        return true;
    }
    bool get(int32_t& out) {
        if (!ensure()) return false;
        auto r = theia::runtime::call<platform_runtime_test_GetReply>(
            ref, platform_runtime_test_Get{}, test::CallAct{0}, /*timeout_ms=*/2000);
        if (r.tag != theia::runtime::CallTag::Reply) return false;
        out = r.reply.value;
        return true;
    }
};
class FrontServer : public theia::runtime::GenServer<FrontServer, FrontState> {
public:
    static constexpr const char* kNodeName = "FrontServer";
    // handle_call: while serving THIS call, make a blocking cross-FC call out.
    platform_runtime_test_GetReply handle_call(const platform_runtime_test_Get&,
                                               FrontState& s) {
        int32_t back = -1;
        bool ok = BackLink::instance().get(back);   // ← the nested RemoteRef call
        platform_runtime_test_GetReply r{};
        r.value = ok ? back : -1;
        s.last = r.value;
        return r;
    }
    void handle_cast(const platform_runtime_test_Inc&, FrontState&) {}
    void handle_info(const char*, FrontState&) {}
};
}  // namespace

static std::string case_nested_remoteref_call() {
    if (::access("/proc/sys/net/tipc", F_OK) != 0) return {};
    namespace rt = theia::runtime;

    // Back server bound at 0xe0010005.
    BackServer back;
    back.start();
    rt::TipcMux back_mux;
    auto* bb = back_mux.bind_node(back, 0xe0010005u, 0u);
    EXPECT(bb != nullptr, "nested: bind back");
    back_mux.register_call<platform_runtime_test_Get,
                           platform_runtime_test_GetReply>(bb, back);
    back_mux.start();

    // Front server bound at 0xe0010004 — its handle_call calls the back server.
    FrontServer front;
    front.start();
    rt::TipcMux front_mux;
    auto* fb = front_mux.bind_node(front, 0xe0010004u, 0u);
    EXPECT(fb != nullptr, "nested: bind front");
    front_mux.register_call<platform_runtime_test_Get,
                            platform_runtime_test_GetReply>(fb, front);
    front_mux.start();

    // A caller hits the FRONT server. The front node thread, while in
    // handle_call, blocks on the BACK RemoteRef — proving a gen_server can make a
    // synchronous cross-FC call from a handler without deadlocking itself.
    rt::RemoteRef<FrontServer, 0xe0010004u, 0u> cref;
    EXPECT(cref.connect(2000), "nested: caller connect");
    front_mux.watch_remote_ref(cref);

    auto r = rt::call<platform_runtime_test_GetReply>(
        cref, platform_runtime_test_Get{}, test::CallAct{0}, /*timeout_ms=*/3000);
    EXPECT(r.tag == rt::CallTag::Reply, "nested: front must reply");
    // The back server started at 100 and ++'d to 101 on the first (nested) call.
    EXPECT(r.reply.value == 101,
           "nested: front must return the BACK server's value (got " +
           std::to_string(r.reply.value) + ", want 101)");

    // Do it again — proves the link is reusable (back → 102).
    auto r2 = rt::call<platform_runtime_test_GetReply>(
        cref, platform_runtime_test_Get{}, test::CallAct{0}, /*timeout_ms=*/3000);
    EXPECT(r2.tag == rt::CallTag::Reply, "nested: 2nd front reply");
    EXPECT(r2.reply.value == 102, "nested: 2nd nested call → back 102 (got " +
           std::to_string(r2.reply.value) + ")");

    front_mux.stop();
    back_mux.stop();
    front.stop();
    back.stop();
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

    namespace rt = theia::runtime;

    TraceCapture cap;

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

    // #401: mark reporting AFTER construction — the GenServer ctor's
    // mark_reporting_() sets it from kReporting (false for this fixture),
    // so we assert it here, post-construct, to exercise the bus submit.
    tr.set_reporting(true);

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

    // Collect the set of TraceKinds seen per corr_id. The fine-grained
    // TraceEvent no longer rides the record (binary record carries the
    // coarse TraceKind only — see #400): send→CastOut, recv→CastIn,
    // dispatch→CallIn, send_reply/call_result→CallOut. The correlation
    // claim now is: ONE non-zero corr_id appears on BOTH an outbound
    // (CastOut) AND an inbound-side (CastIn or CallIn) kind — i.e. the
    // same wire correlation id stitches the send thread to the receive
    // thread over real TIPC.
    std::unordered_map<uint32_t, std::string> kinds_per_corr;
    for (const auto& r : cap.records()) {
        kinds_per_corr[r.corr_id] += std::to_string(r.kind) + ",";
    }
    auto has = [](const std::string& s, rt::TraceKind k) {
        return s.find(std::to_string((int)k) + ",") != std::string::npos;
    };
    bool ok = false;
    for (const auto& kv : kinds_per_corr) {
        if (kv.first == 0) continue;  // synthetic (in-process / non-RPC)
        const auto& s = kv.second;
        if (has(s, rt::TraceKind::CastOut) &&
            (has(s, rt::TraceKind::CastIn) || has(s, rt::TraceKind::CallIn))) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        std::string diag;
        int cnt = 0;
        for (const auto& kv : kinds_per_corr) {
            if (cnt++ >= 5) break;
            diag += "corr=" + std::to_string(kv.first) + ":{" + kv.second + "} ";
        }
        return "no corr_id correlates send→recv across threads. Seen: " + diag;
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
    namespace rt = theia::runtime;

    TraceCapture cap;

    TestServer s;
    s.start();

    auto& tracer = rt::tracer_for(TestServer::kNodeName);
    tracer.set_reporting(true);   // #401: only reporting nodes submit to the bus

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

    // Count submitted records tagged with TestServer. Phase 2 ran one
    // cast (Send + Recv + Dispatch + DispatchDone = 4 events) and one
    // call (4 more) = 8 events total. Phases 1 and 3 produce nothing.
    //
    // We don't pin the exact count — the framework can grow trace points
    // later. Sanity bounds: at least 6, at most ~20.
    int count = 0;
    for (const auto& r : cap.records())
        if (r.node_name == TestServer::kNodeName) ++count;
    if (count < 6 || count > 20) {
        return "expected 6..20 TestServer trace events (one cast + "
               "one call while enabled), got " + std::to_string(count);
    }
    return {};
}

// #355 — per-message-type filter on Tracer.
//
// Direct unit test of Tracer::emit's filter check. Bypasses GenServer:
// we drive Tracer::emit() with known msg_type strings and verify that
// emit drops events whose msg type isn't in the filter. This is the
// runtime hook the supervisor's NodeTraceCtl push (#361) will drive
// on each child via the daemon's trace_enable() method.
//
// Three subcases:
//   (1) filter empty + enabled → emit fires (back-compat).
//   (2) filter set to {Foo} + enabled → emit("Foo", ...) fires, emit("Bar", ...) drops.
//   (3) trace_clear_all → returns to subcase (1) behavior.
static std::string case_tracer_msg_type_filter() {
    namespace rt = theia::runtime;

    TraceCapture cap;

    // Fresh per-test tracer (different name → singleton not shared
    // with other cases). enable + start clean.
    auto& tracer = rt::tracer_for("FilterTestNode");
    tracer.enable(true);
    tracer.set_reporting(true);   // #401: submit gate — exercise the bus path
    tracer.trace_clear_all();

    // (1) filter empty: everything passes.
    tracer.emit(rt::TraceEvent::Send, "Foo", 0, nullptr, 0);
    tracer.emit(rt::TraceEvent::Send, "Bar", 0, nullptr, 0);

    // (2) filter set: only Foo passes.
    tracer.trace_enable("Foo", true);
    tracer.emit(rt::TraceEvent::Send, "Foo", 1, nullptr, 0);  // pass
    tracer.emit(rt::TraceEvent::Send, "Bar", 2, nullptr, 0);  // drop
    tracer.emit(rt::TraceEvent::Send, "Baz", 3, nullptr, 0);  // drop

    // (3) clear filter: back to all-pass.
    tracer.trace_clear_all();
    tracer.emit(rt::TraceEvent::Send, "Bar", 4, nullptr, 0);  // pass
    tracer.emit(rt::TraceEvent::Send, "Baz", 5, nullptr, 0);  // pass

    // Cleanup.
    tracer.enable(false);

    // Count submitted records matching (msg_type, corr_id). corr=0 means
    // proto3 omitted the field (default) — decode_trace_record leaves it 0.
    auto recs = cap.records();
    auto count = [&](const char* msg, uint32_t corr) {
        int n = 0;
        for (const auto& r : recs)
            if (r.msg_type == msg && r.corr_id == corr) ++n;
        return n;
    };

    // (1) two unfiltered records:
    if (count("Foo", 0) != 1)
        return "(1) expected Foo corr=0, records=" + std::to_string(recs.size());
    if (count("Bar", 0) != 1)
        return "(1) expected Bar corr=0";

    // (2) only Foo passes; Bar and Baz drop:
    if (count("Foo", 1) != 1)
        return "(2) Foo corr=1 should pass";
    if (count("Bar", 2) != 0)
        return "(2) Bar corr=2 should drop (filter restricted)";
    if (count("Baz", 3) != 0)
        return "(2) Baz corr=3 should drop (filter restricted)";

    // (3) cleared filter, all pass again:
    if (count("Bar", 4) != 1)
        return "(3) Bar corr=4 should pass after clear";
    if (count("Baz", 5) != 1)
        return "(3) Baz corr=5 should pass after clear";

    return {};
}

// The trace KIND filter is a BITMASK (#403): a node can trace several
// dispatch classes at once (CastOut | CallIn | ...). This proves the mask
// accumulates, drops un-selected kinds, supports per-bit clear without
// nuking the others, and that mask==0 is the catch-all "all kinds pass".
// The Tracer methods exercised here are the ones the supervisor's
// TraceControlPush handle_cast drives (GenServer.hh).
static std::string case_tracer_kind_mask() {
    namespace rt = theia::runtime;
    TraceCapture cap;

    auto& tr = rt::tracer_for("KindMaskTestNode");
    tr.enable(true);
    tr.set_reporting(true);
    tr.trace_clear_all();      // no msg_type filter — isolate the kind filter
    tr.trace_clear_kinds();    // mask 0 = catch-all

    // TraceEvent → TraceKind: Send=CastOut, Recv=CastIn, Dispatch=CallIn,
    // SendReply=CallOut, StateTransition=Statem.
    // (0) mask empty → every kind passes.
    tr.emit(rt::TraceEvent::Send,    "A", 0, nullptr, 0);  // CastOut, pass
    tr.emit(rt::TraceEvent::Recv,    "A", 0, nullptr, 0);  // CastIn,  pass

    // (1) narrow to CastOut | CallIn — accumulate two bits.
    tr.trace_enable_kind(rt::TraceKind::CastOut, true);
    tr.trace_enable_kind(rt::TraceKind::CallIn,  true);
    tr.emit(rt::TraceEvent::Send,     "B", 1, nullptr, 0);  // CastOut, pass
    tr.emit(rt::TraceEvent::Dispatch, "B", 1, nullptr, 0);  // CallIn,  pass
    tr.emit(rt::TraceEvent::Recv,     "B", 1, nullptr, 0);  // CastIn,  DROP
    tr.emit(rt::TraceEvent::SendReply,"B", 1, nullptr, 0);  // CallOut, DROP

    // (2) clear just CastOut — CallIn must survive.
    tr.trace_enable_kind(rt::TraceKind::CastOut, false);
    tr.emit(rt::TraceEvent::Send,     "C", 2, nullptr, 0);  // CastOut, DROP
    tr.emit(rt::TraceEvent::Dispatch, "C", 2, nullptr, 0);  // CallIn,  pass

    // (3) clear kinds → back to catch-all, all pass.
    tr.trace_clear_kinds();
    tr.emit(rt::TraceEvent::Recv,     "D", 3, nullptr, 0);  // CastIn,  pass
    tr.emit(rt::TraceEvent::SendReply,"D", 3, nullptr, 0);  // CallOut, pass

    tr.enable(false);

    auto recs = cap.records();
    auto count = [&](const char* msg, uint32_t corr) {
        int n = 0;
        for (const auto& r : recs)
            if (r.msg_type == msg && r.corr_id == corr) ++n;
        return n;
    };

    // (0) catch-all: both pass.
    if (count("A", 0) != 2)
        return "(0) catch-all should pass both A, got "
               + std::to_string(count("A", 0));
    // (1) only the two selected kinds pass (CastOut + CallIn = 2 of 4).
    if (count("B", 1) != 2)
        return "(1) mask CastOut|CallIn should pass exactly 2 of 4 B, got "
               + std::to_string(count("B", 1));
    // (2) CastOut cleared, CallIn survives → only the Dispatch passes.
    if (count("C", 2) != 1)
        return "(2) after clearing CastOut, only CallIn passes, got "
               + std::to_string(count("C", 2));
    // (3) catch-all again: both pass.
    if (count("D", 3) != 2)
        return "(3) cleared mask should pass both D, got "
               + std::to_string(count("D", 3));
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

// =======================================================================
//                              GenStateM CASES
// =======================================================================

static std::string case_statem_traffic_light() {
    using namespace test_statem;
    rt::TimerService timers;
    TrafficLight tl;
    tl.start_statem(timers);

    // Wait briefly for init() + first on_enter (Red).
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT(tl.state().state == Light::Red, "should init to Red");
    EXPECT(tl.state().data.history.size() == 1,
           "on_enter fires once on init");

    // Kick off the cycle.
    rt::post_event(tl, TLBoot{});

    // Three full cycles: Red→Green→Yellow→Red→Green→Yellow→Red→Green→Yellow.
    // Each cycle 90ms. Wait 350ms for 3 cycles + slack.
    std::this_thread::sleep_for(std::chrono::milliseconds(330));

    // ticks counts state-timeout fires (we get one per transition).
    int ticks = tl.state().data.ticks.load();
    EXPECT(ticks >= 8,
           "expected at least 8 state-timeout ticks in 330ms, got " +
               std::to_string(ticks));

    // History should be [Red, Green, Yellow, Red, Green, Yellow, ...].
    const auto& hist = tl.state().data.history;
    EXPECT(hist.size() >= 9, "history has at least 9 entries");
    EXPECT(hist[0] == Light::Red,    "history[0]=Red");
    EXPECT(hist[1] == Light::Green,  "history[1]=Green");
    EXPECT(hist[2] == Light::Yellow, "history[2]=Yellow");
    EXPECT(hist[3] == Light::Red,    "history[3]=Red");

    tl.stop();
    return {};
}

static std::string case_statem_retry_escalate() {
    using namespace test_statem;
    rt::TimerService timers;
    RetryEscalator re;
    re.start_statem(timers);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT(re.state().state == Retry::Trying, "init to Trying");

    // First two retries — counter advances, state stays Trying.
    rt::post_event(re, DoRetry{});
    rt::post_event(re, DoRetry{});
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT(re.state().data.retry_count == 2, "two retries logged");
    EXPECT(re.state().state == Retry::Trying, "still Trying");
    EXPECT(!re.state().data.escalated, "not yet escalated");

    // Send Resume — handled (resume_count increments).
    rt::post_event(re, Resume{});
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT(re.state().data.resume_count == 1,
           "Resume handled in Trying");

    // Third retry escalates.
    rt::post_event(re, DoRetry{});
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT(re.state().data.escalated, "escalated on 3rd retry");
    EXPECT(re.state().state == Retry::Failed, "now in Failed");

    re.stop();
    return {};
}

static std::string case_statem_postpone() {
    // Resume sent in Failed is postponed and replayed when we re-enter
    // Trying. We have no "go back to Trying" event in the FSM, so this
    // test contrives one: a second instance where we drive Resume first
    // (gets postponed), then a custom hook transitions Failed→Trying
    // and we observe resume_count increments AFTER that.
    using namespace test_statem;
    rt::TimerService timers;
    RetryEscalator re;
    re.start_statem(timers);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Drive to Failed.
    rt::post_event(re, DoRetry{});
    rt::post_event(re, DoRetry{});
    rt::post_event(re, DoRetry{});
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT(re.state().state == Retry::Failed, "in Failed");

    // Send Resume — should be postponed (no resume_count yet).
    rt::post_event(re, Resume{});
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT(re.state().data.resume_count == 0,
           "Resume in Failed is postponed, not handled");

    // Synthesize a manual transition back to Trying by going through
    // the dispatcher: we can't from outside, so we tap the holder. To
    // keep the test honest, we use `enqueue` to push a lambda that
    // does the transition + postpone-replay just like the framework.
    re.enqueue([](rt::GenServerBase* base) {
        auto* self = static_cast<RetryEscalator*>(base);
        auto& h    = self->state();
        h.expected_cookie++;
        h.state = Retry::Trying;
        // Replay postponed events.
        while (!h.postponed.empty()) {
            auto fn = std::move(h.postponed.front());
            h.postponed.pop_front();
            base->enqueue(std::move(fn));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT(re.state().state == Retry::Trying, "back to Trying");
    EXPECT(re.state().data.resume_count == 1,
           "postponed Resume replayed → resume_count=1");

    re.stop();
    return {};
}

static std::string case_statem_door_lock_guards() {
    using namespace test_statem;
    rt::TimerService timers;
    DoorLock dl;
    dl.start_statem(timers);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT(dl.state().state == Door::Locked, "init Locked");

    // PresentCard without a registered keycard → rejected, stays Locked.
    rt::post_event(dl, PresentCard{});
    rt::post_event(dl, PresentCard{});
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT(dl.state().data.reject_count == 2, "rejected twice");
    EXPECT(dl.state().state == Door::Locked, "still Locked");

    // Register a keycard.
    rt::post_event(dl, InsertKey{true});
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT(dl.state().data.has_keycard, "keycard now valid");

    // PresentCard now transitions to Unlocking with a 50ms timeout.
    rt::post_event(dl, PresentCard{});
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT(dl.state().state == Door::Unlocking, "in Unlocking");

    // After the timeout fires → Unlocked.
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    EXPECT(dl.state().state == Door::Unlocked, "now Unlocked");
    EXPECT(dl.state().data.unlock_count == 1, "unlocked once");

    dl.stop();
    return {};
}

// ---- driver ------------------------------------------------------------

int main() {
    TestStat stat;

    CASE(stat, call_basic) { return case_call_basic(); });
    CASE(stat, call_sync_timeout) { return case_call_sync_timeout(); });
    CASE(stat, cast_basic) { return case_cast_basic(); });
    CASE(stat, cast_returns_immediately) { return case_cast_returns_immediately(); });
    CASE(stat, feed_conflation_backlog) { return case_feed_conflation_backlog(); });
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
    CASE(stat, tipc_remoteref_churn)    { return case_tipc_remoteref_churn(); });
    CASE(stat, nested_remoteref_call)   { return case_nested_remoteref_call(); });
    CASE(stat, tipc_trace_correlation)  { return case_tipc_trace_correlation(); });
    CASE(stat, tracer_runtime_toggle)   { return case_tracer_runtime_toggle(); });
    CASE(stat, tracer_msg_type_filter)  { return case_tracer_msg_type_filter(); });
    CASE(stat, tracer_kind_mask)        { return case_tracer_kind_mask(); });
    CASE(stat, statem_traffic_light)    { return case_statem_traffic_light(); });
    CASE(stat, statem_retry_escalate)   { return case_statem_retry_escalate(); });
    CASE(stat, statem_postpone)         { return case_statem_postpone(); });
    CASE(stat, statem_door_lock_guards) { return case_statem_door_lock_guards(); });

    std::printf("\n%d/%d passed\n", stat.passed, stat.total);
    if (!stat.failures.empty()) {
        std::printf("\nFailures:\n");
        for (const auto& f : stat.failures) std::printf("  ✗ %s\n", f.c_str());
        return 1;
    }
    return 0;
}

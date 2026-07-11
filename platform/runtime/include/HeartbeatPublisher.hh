// HeartbeatPublisher — periodic node-liveness reports to the supervisor.
//
// Adaptive AUTOSAR's Platform Health Manager owns "alive supervision": every
// REPORTING node emits a heartbeat at a known cadence; the supervisor watchdog
// SIGTERMs nodes that miss K consecutive deadlines (check_heartbeats() in
// platform/supervisor/impl/core/runtime.cpp, kMaxAge=3s). This is the publisher
// half — owned by the generated main.cc, started once per reporting node
// (GenServer AND GenRunnable alike). The watchdog is beat-triggered: it only
// watches a node that has EVER beat, so a non-reporting / passive node stays
// exempt.
//
// ---- why hand-framed (no supervisor proto dependency) -------------------
//
// The beat is a standard GEN_CAST of system_supervisor.HeartbeatReport to the
// supervisor's NodeReportIf receiver (SupervisorCtl, TIPC 0x80020001/0), keyed
// by service_id = djb2_low16("system_supervisor_HeartbeatReport") — the same
// id the supervisor's register_cast<HeartbeatReport> binds, so it matches by
// construction. We hand-encode the four proto3 fields + frame a TheiaMsgHeader
// here rather than pull the supervisor's .pb.h in, because:
//
//   * platform/runtime is BELOW the supervisor proto in the build graph — the
//     supervisor proto DEPENDS ON //platform/runtime:runtime_proto_src, so a
//     runtime→supervisor-proto edge would be a CYCLE. HeartbeatPublisher.hh is
//     part of runtime:runtime's hdrs glob, so it must not #include it.
//   * The HeartbeatReport schema is 4 stable scalar fields; hand-encoding is
//     trivial and exactly the pattern Tracer.hh's TraceSubmitter already uses
//     to frame a TraceRecord cast without nanopb in the hot path.
//
// monotonic_ns is the node's CLOCK_MONOTONIC at emit; the supervisor compares
// to its own clock for skew. seq is monotonic per node — gaps == missed sends.
//
// Usage in a generated main(), after the node's start():
//
//     theia::runtime::HeartbeatPublisher hb(MyNode::kNodeName);
//     if (hb.open()) hb.start(/*period_ms=*/1000);
//
// open() returns false on hosts without AF_TIPC (no module / no permission);
// the publisher is then inert and the node runs normally.

#pragma once

#include "TheiaMsgHeader.hh"   // TheiaMsgHeader + kBusTypeRpc / kMsgGenCast
#include "RemoteCodec.hh"      // hash_msg_type_ — service_id derivation

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <linux/tipc.h>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

namespace theia { namespace runtime {

class HeartbeatPublisher {
public:
    // The supervisor's control node (SupervisorCtl / NodeReportIf receiver).
    static constexpr uint32_t kSupTipcType     = 0x80020001u;
    static constexpr uint32_t kSupTipcInstance = 0u;

    // The on-wire message type name — service_id is its djb2_low16, matching
    // the supervisor's register_cast<HeartbeatReport>(...) (which keys on the
    // SAME THEIA_DECLARE_REMOTE_CODEC(system_supervisor_HeartbeatReport)).
    static constexpr const char* kMsgTypeName =
        "system_supervisor_HeartbeatReport";
    static constexpr uint16_t kServiceId = hash_msg_type_(kMsgTypeName);

    // The initial connect retries this many times on fresh sockets so it lands
    // on the LIVE supervisor binding even when SIGKILL'd predecessors left stale
    // bindings the kernel hasn't reaped (connect-by-name load-balances across
    // all of them). Once connected we STAY (no periodic re-roll).
    static constexpr int kConnectTries = 6;

    explicit HeartbeatPublisher(std::string node_name)
        : node_name_(std::move(node_name)), self_pid_(::getpid()) {}

    ~HeartbeatPublisher() { stop(); close(); }

    HeartbeatPublisher(const HeartbeatPublisher&)            = delete;
    HeartbeatPublisher& operator=(const HeartbeatPublisher&) = delete;

    // Open a SEQPACKET socket. Succeeds as long as AF_TIPC is available — the
    // CONNECT to the supervisor is LAZY (done in send_once, retried every tick)
    // so a node that starts before / racing the supervisor's bind still beats
    // once the supervisor is up. Returns false only when AF_TIPC itself is
    // unavailable (no kernel module / no permission), leaving the node inert.
    bool open() {
        if (fd_ < 0)
            fd_ = ::socket(AF_TIPC, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return false;
        connected_ = connect_locked_();   // best-effort; send_once retries
        return true;
    }

    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    // Cast one HeartbeatReport now. Best-effort: a send failure (supervisor
    // restarting / not yet bound) is silently dropped; the next tick retries.
    void send_once() {
        if (fd_ < 0) return;
        // Lazy connect, then STAY connected. A TIPC connect-by-name picks ONE
        // port at connect time (load-balancing across every binding at the
        // supervisor's name); once a SEQPACKET connection is ESTABLISHED it is
        // pinned to THAT port — subsequent sends do NOT re-roll the dice. So the
        // ONLY danger is the connect landing on a STALE binding left by a
        // SIGKILL'd predecessor (the kernel reaps those slowly): a send to a
        // dead port silently succeeds, the supervisor never hears us, and its
        // watchdog SIGTERMs a healthy node.
        //
        // We therefore do NOT periodically re-roll (the old behavior — it broke
        // a WORKING connection every N beats and re-rolled onto a possibly-dead
        // binding, which is exactly what killed healthy nodes at multiples of
        // the re-roll period). Instead the connect itself is the careful step:
        // we keep the established connection forever, and only re-connect when
        // the peer actually goes away (the send below fails → supervisor
        // restarted → reconnect lands on the FRESH binding). To survive a connect
        // that lands on a stale binding at startup, the FIRST connect retries a
        // few times on fresh sockets (kConnectTries) — over a few tries it lands
        // on the live binding, and once connected it stays.
        // (project-probe-connect-stale-bindings.)
        if (!connected_) {
            for (int i = 0; i < kConnectTries && !connected_; ++i) {
                if (i > 0) {                      // fresh socket per retry
                    ::close(fd_);
                    fd_ = ::socket(AF_TIPC, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
                    if (fd_ < 0) return;
                }
                connected_ = connect_locked_();
            }
            if (!connected_) return;
        }
        std::string rec;
        rec.reserve(48);
        pb_string(rec, 1, node_name_.c_str(), node_name_.size());   // node_name
        // pid is sint32 (zigzag); the supervisor decodes the same encoding.
        pb_varint_field(rec, 2, zigzag32(self_pid_));               // pid
        pb_varint_field(rec, 3, ++seq_);                            // seq
        pb_varint_field(rec, 4, monotonic_ns());                   // monotonic_ns

        TheiaMsgHeader hdr{};
        hdr.bus_type        = kBusTypeRpc;
        hdr.msg_type        = kMsgGenCast;
        hdr.proto_len       = static_cast<uint16_t>(rec.size());
        hdr.rpc.service_id  = kServiceId;
        hdr.rpc.method_id   = 0;
        hdr.rpc.correlation_id = 0;
        std::string frame(sizeof(hdr) + rec.size(), '\0');
        std::memcpy(&frame[0], &hdr, sizeof(hdr));
        std::memcpy(&frame[sizeof(hdr)], rec.data(), rec.size());
        if (::send(fd_, frame.data(), frame.size(),
                   MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {
            // Peer gone (supervisor restart): drop + re-create the socket and
            // mark unconnected, so the next tick's lazy connect re-binds to the
            // fresh supervisor name.
            ::close(fd_);
            fd_ = ::socket(AF_TIPC, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
            connected_ = false;
        }
    }

    void start(uint32_t period_ms) {
        if (fd_ < 0) return;
        if (running_.exchange(true)) return;
        period_ms_ = period_ms;
        thread_ = std::thread([this] {
            std::unique_lock<std::mutex> lk(wake_mu_);
            while (running_.load()) {
                send_once();
                // Interruptible sleep: wait the period OR until stop() notifies.
                // A plain sleep_for(1s) makes shutdown take up to a full period
                // per node (3 reporting nodes × 1s ≈ the 3s stop stall this
                // fixes); wait_for wakes immediately on the stop predicate.
                wake_cv_.wait_for(lk, std::chrono::milliseconds(period_ms_),
                                  [this] { return !running_.load(); });
            }
        });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        wake_cv_.notify_all();        // break the period wait now, don't wait it out
        if (thread_.joinable()) thread_.join();
    }

private:
    bool connect_locked_() {
        struct sockaddr_tipc addr{};
        addr.family                  = AF_TIPC;
        addr.addrtype                = TIPC_ADDR_NAME;
        addr.addr.name.name.type     = kSupTipcType;
        addr.addr.name.name.instance = kSupTipcInstance;
        addr.scope                   = TIPC_NODE_SCOPE;
        return ::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr),
                         sizeof(addr)) == 0;
    }

    static uint64_t monotonic_ns() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
               static_cast<uint64_t>(ts.tv_nsec);
    }

    // proto3 sint32 zigzag (field is `sint32 pid`).
    static uint64_t zigzag32(int32_t v) {
        return static_cast<uint32_t>((v << 1) ^ (v >> 31));
    }

    // ── minimal proto3 wire encoders (same shape as Tracer.hh) ──────────
    static void pb_varint(std::string& out, uint64_t v) {
        while (v >= 0x80) { out.push_back(char((v & 0x7f) | 0x80)); v >>= 7; }
        out.push_back(static_cast<char>(v));
    }
    static void pb_tag(std::string& out, uint32_t field, uint32_t wire) {
        pb_varint(out, (static_cast<uint64_t>(field) << 3) | wire);
    }
    static void pb_varint_field(std::string& out, uint32_t field, uint64_t v) {
        pb_tag(out, field, 0); pb_varint(out, v);
    }
    static void pb_string(std::string& out, uint32_t field,
                          const char* s, size_t len) {
        if (!s || !len) return;
        pb_tag(out, field, 2); pb_varint(out, len); out.append(s, len);
    }

    std::string           node_name_;
    int                   fd_       = -1;
    bool                  connected_ = false;  // lazy-connect state (one thread)
    std::atomic<uint64_t> seq_      {0};
    pid_t                 self_pid_ = -1;
    std::atomic<bool>     running_  {false};
    std::thread           thread_;
    uint32_t              period_ms_ = 0;
    std::mutex                wake_mu_;   // guards the interruptible period wait
    std::condition_variable   wake_cv_;   // stop() notifies to break the wait
};

}}  // namespace theia::runtime

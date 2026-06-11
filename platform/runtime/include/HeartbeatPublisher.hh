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
#include <cstdint>
#include <cstring>
#include <ctime>
#include <linux/tipc.h>
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

    explicit HeartbeatPublisher(std::string node_name)
        : node_name_(std::move(node_name)), self_pid_(::getpid()) {}

    ~HeartbeatPublisher() { stop(); close(); }

    HeartbeatPublisher(const HeartbeatPublisher&)            = delete;
    HeartbeatPublisher& operator=(const HeartbeatPublisher&) = delete;

    // Open a SEQPACKET socket connected to the supervisor's control name.
    // Returns false if AF_TIPC is unavailable or the supervisor isn't bound;
    // a later (re)connect happens lazily inside send_once on first failure.
    bool open() {
        fd_ = ::socket(AF_TIPC, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return false;
        if (!connect_locked_()) { ::close(fd_); fd_ = -1; return false; }
        return true;
    }

    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    bool is_open() const { return fd_ >= 0; }

    // Cast one HeartbeatReport now. Best-effort: a send failure (supervisor
    // restarting) is silently dropped; the next tick retries.
    void send_once() {
        if (fd_ < 0) return;
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
            // Peer gone (supervisor restart): drop the socket so the next tick
            // reconnects to the fresh supervisor binding.
            ::close(fd_);
            fd_ = ::socket(AF_TIPC, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
            if (fd_ >= 0 && !connect_locked_()) { ::close(fd_); fd_ = -1; }
        }
    }

    void start(uint32_t period_ms) {
        if (fd_ < 0) return;
        if (running_.exchange(true)) return;
        period_ms_ = period_ms;
        thread_ = std::thread([this] {
            while (running_.load()) {
                send_once();
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(period_ms_));
            }
        });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
    }

    uint64_t seq() const { return seq_.load(); }

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
    std::atomic<uint64_t> seq_      {0};
    pid_t                 self_pid_ = -1;
    std::atomic<bool>     running_  {false};
    std::thread           thread_;
    uint32_t              period_ms_ = 0;
};

}}  // namespace theia::runtime

// demo::runtime::Tracer — runtime-toggleable per-node-type tracing.
//
// Producer (the node's framework dispatch points) emits a small trace
// record on each interesting event. Records are tiny by design: a
// header + the raw nanopb wire bytes for the message. Decoding and
// retention is a separate concern handled by a trace collector
// (out of scope for the demo — we write to stderr for now).
//
// Cost model:
//   * disabled (default): one std::atomic<bool> relaxed load + branch.
//     Predicted not-taken. ~1 ns on modern x86.
//   * enabled: one stack-allocated formatting pass + one fputs to stderr.
//     For in-process events that didn't already have wire bytes, the
//     producer encodes the message via nanopb just for the trace —
//     paid only when enabled.
//
// Why no compile-time gate: the most valuable trace use case is the
// supervisor flipping it on AFTER a crash to capture the next
// occurrence. Stamping it out for "system service" nodes throws away
// that capability. The ~kB of code we'd save isn't worth the loss.
//
// Tracer instances are per node-type-name (e.g. "DriverNode"), not
// per-instance: enabling traces all DriverNodes in the process at once.
// Matches Erlang's `sys:trace(my_server, true)` model.

#pragma once

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstdlib>   // getenv — THEIA_TRACE boot switch
#include <cstring>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <linux/tipc.h>     // trace submit rides TIPC SOCK_DGRAM
#include <sys/socket.h>
#include <unistd.h>

#include "gw_proto.h"       // GwMessageHeader / GW_MSG_GEN_CAST framing

namespace demo {
namespace runtime {

// Event kinds. Numeric so the trace record can carry them as a byte
// without name strings.
enum class TraceEvent : uint8_t {
    Send         = 0x01,  // outbound cast or call request being sent
    SendReply    = 0x02,  // outbound call reply being sent (server side)
    Recv         = 0x03,  // inbound message landed in mailbox
    Dispatch     = 0x04,  // handler invocation start
    DispatchDone = 0x05,  // handler invocation end (return)
    Info         = 0x06,  // handle_info(const char*) called
    Terminate    = 0x07,  // node shutdown — terminate() called
    CallResult   = 0x08,  // caller-side handle_call_result
    CallTimeout  = 0x09,  // caller-side handle_call_timeout
    CallError    = 0x0a,  // caller-side handle_call_error
    CallWait     = 0x0b,  // sync caller about to block on a future
    CallResume   = 0x0c,  // sync caller unblocked (reply arrived or timeout)
    StateTransition = 0x0d,  // gen_statem transitioned between states
    StateTimeout    = 0x0e,  // gen_statem state-timeout fired
};

inline const char* trace_event_name(TraceEvent e) noexcept {
    switch (e) {
        case TraceEvent::Send:         return "send";
        case TraceEvent::SendReply:    return "send_reply";
        case TraceEvent::Recv:         return "recv";
        case TraceEvent::Dispatch:     return "dispatch";
        case TraceEvent::DispatchDone: return "dispatch_done";
        case TraceEvent::Info:         return "info";
        case TraceEvent::Terminate:    return "terminate";
        case TraceEvent::CallResult:   return "call_result";
        case TraceEvent::CallTimeout:  return "call_timeout";
        case TraceEvent::CallError:    return "call_error";
        case TraceEvent::CallWait:     return "call_wait";
        case TraceEvent::CallResume:   return "call_resume";
        case TraceEvent::StateTransition: return "state_transition";
        case TraceEvent::StateTimeout:    return "state_timeout";
    }
    return "?";
}

// ── TraceKind: the on-wire trace_kind (services_services_log.TraceKind) ──
//
// Mirrors the .art enum (services/log/system/package.art). The runtime
// produces these by collapsing the richer TraceEvent matrix below.
// Numbers MUST match the .art enum — they ride field 6 of the record.
enum class TraceKind : uint8_t {
    Other   = 0,
    CastOut = 1,
    CastIn  = 2,
    CallOut = 3,
    CallIn  = 4,
    Statem  = 5,
};

// Map the fine-grained TraceEvent onto the coarse TraceKind the consumer
// filters by. call/cast × in/out, plus statem; everything else → Other.
inline TraceKind trace_kind_of(TraceEvent e) noexcept {
    switch (e) {
        case TraceEvent::Send:         return TraceKind::CastOut;   // outbound cast/req
        case TraceEvent::Recv:         return TraceKind::CastIn;    // inbound landed
        case TraceEvent::Dispatch:     return TraceKind::CallIn;    // handler entry (call/cast)
        case TraceEvent::SendReply:    return TraceKind::CallOut;   // reply sent (server)
        case TraceEvent::CallResult:
        case TraceEvent::CallWait:
        case TraceEvent::CallResume:   return TraceKind::CallOut;   // caller-side call lifecycle
        case TraceEvent::StateTransition:
        case TraceEvent::StateTimeout: return TraceKind::Statem;
        default:                       return TraceKind::Other;     // Info/Terminate/DispatchDone/errors
    }
}

// ── TraceSubmitter: one process-wide TIPC SOCK_DGRAM client ──────────────
//
// The trace egress producer (the missing half — see
// docs/tasks/BACKLOG/trace-to-rf-via-com.md). When a node's Tracer is
// enabled, emit() frames a TraceRecord and casts it to the log[trace]
// collector's in_records port. SOCK_DGRAM, best-effort: a slow or absent
// collector must NEVER block or back-pressure the traced dispatch thread,
// and dropping a trace record is fine. One socket per process, guarded by
// a short mutex (emit() runs on many node threads).
class TraceSubmitter {
public:
    // log[trace] TraceCollector — services/log/system/package.art.
    static constexpr uint32_t kCollectorTipcType     = 0x80010013u;
    static constexpr uint32_t kCollectorTipcInstance = 0u;

    // service_id the collector's in_records registers: djb2_low16 of the
    // nanopb C type name "services_services_log_TraceRecord". The runtime
    // submits under THAT name (the on-wire contract), regardless of what
    // it calls the type internally. Keep in sync with RemoteCodec's djb2.
    static constexpr uint16_t kRecordServiceId = 0xb17au;

    static TraceSubmitter& instance() {
        static TraceSubmitter s;
        return s;
    }

    // Test seam: when set, every encoded TraceRecord wire blob is handed
    // to this sink INSTEAD of going out over TIPC. Lets tests assert on
    // the real encoded record without needing AF_TIPC in the sandbox.
    // Production never sets it. Thread-safe (held under the same mutex).
    using TestSink = std::function<void(const std::string&)>;
    void set_test_sink(TestSink sink) noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        test_sink_ = std::move(sink);
    }

    // Frame [GwMessageHeader|GEN_CAST][proto-wire TraceRecord] and send it.
    // Lossy by design — returns void; any failure is silently dropped.
    void submit(const std::string& record_wire) noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        if (test_sink_) { test_sink_(record_wire); return; }
        if (!ensure_open_locked()) return;
        GwMessageHeader hdr{};
        hdr.bus_type           = GW_BUS_TYPE_RPC;
        hdr.msg_type           = GW_MSG_GEN_CAST;
        hdr.proto_len          = static_cast<uint16_t>(record_wire.size());
        hdr.rpc.service_id     = kRecordServiceId;
        hdr.rpc.method_id      = 0;
        hdr.rpc.correlation_id = 0;
        // One datagram = header + payload (contiguous).
        std::vector<uint8_t> frame(sizeof(hdr) + record_wire.size());
        std::memcpy(frame.data(), &hdr, sizeof(hdr));
        std::memcpy(frame.data() + sizeof(hdr),
                    record_wire.data(), record_wire.size());
        // MSG_DONTWAIT: never block the dispatch thread; drop on EWOULDBLOCK.
        (void)::sendto(fd_, frame.data(), frame.size(),
                       MSG_NOSIGNAL | MSG_DONTWAIT,
                       reinterpret_cast<struct sockaddr*>(&addr_),
                       sizeof(addr_));
    }

private:
    TraceSubmitter() {
        addr_.family                  = AF_TIPC;
        addr_.addrtype                = TIPC_ADDR_NAME;
        addr_.addr.name.name.type     = kCollectorTipcType;
        addr_.addr.name.name.instance = kCollectorTipcInstance;
        addr_.scope                   = TIPC_NODE_SCOPE;
    }
    ~TraceSubmitter() { if (fd_ >= 0) ::close(fd_); }

    bool ensure_open_locked() noexcept {
        if (fd_ >= 0) return true;
        // SOCK_DGRAM: connectionless, lossy-OK firehose (per the locked
        // design). No connect() — we sendto() the collector's service name
        // each datagram, so the socket needs no live peer at open time and
        // survives the collector (re)starting underneath us.
        fd_ = ::socket(AF_TIPC, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        return fd_ >= 0;
    }

    std::mutex          mu_;
    int                 fd_ = -1;
    struct sockaddr_tipc addr_{};
    TestSink            test_sink_;
};

class Tracer {
public:
    explicit Tracer(const char* node_name)
        : name_(node_name),
          start_tp_(std::chrono::steady_clock::now()) {}

    bool enabled() const noexcept {
        return enabled_.load(std::memory_order_relaxed);
    }
    void enable(bool on) noexcept {
        enabled_.store(on, std::memory_order_relaxed);
    }

    // Reporting gate (#401). Only AUTOSAR reporting nodes submit trace
    // records to the collector over the bus — matching how the
    // supervisor's NodeTraceCtl config receiver is injected only for
    // reporting nodes. The GenServer/GenStateM base sets this from
    // Derived::kReporting at start(); a Tracer nobody marked stays false
    // (safe default: no bus traffic from an unmarked tracer). A
    // non-reporting node may still have enabled_ flipped (e.g. by the
    // THEIA_TRACE=1 boot switch) but its emit() skips the submit.
    void set_reporting(bool on) noexcept {
        reporting_.store(on, std::memory_order_relaxed);
    }
    bool reporting() const noexcept {
        return reporting_.load(std::memory_order_relaxed);
    }

    // Per-msg-type filter (#355). Two-tier gate:
    //
    //   enabled_ == false  → emit nothing (master off).
    //   enabled_ == true + filter empty → emit everything (master on,
    //                                     no filter set; back-compat).
    //   enabled_ == true + filter set   → emit ONLY msg types listed
    //                                     in the filter.
    //
    // Used by the supervisor's ConfigureTrace path: supdbg picks the
    // msg types it wants and pushes them via TIPC to the node, the
    // node's `trace_enable("SmStateMsg", true)` delegates to
    // Tracer::trace_enable, and subsequent emit()s for that type land
    // on the wire while everything else stays silent.
    //
    // Filter ops take a short lock; emit's filter check uses the same
    // lock. The cost is ~30 ns per check — only paid when enabled_ is
    // already true, so it's invisible to the disabled fast path.
    void trace_enable(const char* msg_type, bool on) noexcept {
        std::lock_guard<std::mutex> lk(filter_mu_);
        if (on) {
            filter_[msg_type] = true;
        } else {
            filter_.erase(msg_type);
        }
    }
    void trace_clear_all() noexcept {
        std::lock_guard<std::mutex> lk(filter_mu_);
        filter_.clear();
    }
    bool trace_filter_passes(const char* msg_type) const noexcept {
        std::lock_guard<std::mutex> lk(
            const_cast<std::mutex&>(filter_mu_));
        if (filter_.empty()) return true;  // back-compat: no filter = all
        if (msg_type == nullptr) return true;  // events w/o a type (rare)
        auto it = filter_.find(msg_type);
        return it != filter_.end() && it->second;
    }

    // Hot path. Caller MUST have already checked enabled() — we don't
    // re-check, to make the cost model explicit at call sites.
    // payload may be nullptr / 0-len for events without typed data.
    // corr_id pairs Send/Recv across processes (for RPCs) or pairs
    // CallWait/CallResume locally; 0 means "no correlation".
    //
    // Filter check happens here (inside emit, not at the call site) so
    // that all existing call sites — Tracer::emit(...) called from
    // dozens of dispatch points across GenServer/TipcMux/RemoteRef —
    // pick up filtering automatically without an audit.
    void emit(TraceEvent kind,
              const char* msg_type_name,
              uint32_t       corr_id,
              const uint8_t* payload, uint16_t payload_len) noexcept {
        if (!trace_filter_passes(msg_type_name)) return;
        // Reporting gate (#401): non-reporting nodes never reach the bus,
        // even if enabled_ was flipped by the THEIA_TRACE boot switch.
        if (!reporting_.load(std::memory_order_relaxed)) return;
        auto now = std::chrono::steady_clock::now();
        uint64_t ts_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now - start_tp_).count());

        // Frame the record as proto3 wire bytes (TraceRecord, fields per
        // services/log/system/package.art) and submit over TIPC. We
        // hand-encode rather than pull nanopb's pb_callback_t machinery
        // into this hot header (see the BACKLOG design doc). dst is left
        // empty: only send sites know a peer, and no call site passes one
        // yet — "" is the .art-sanctioned no-peer value.
        //   1=node_name(src) 2=dst 3=msg_type 4=corr_id 5=ts_ns
        //   6=kind 7=payload
        std::string rec;
        rec.reserve(64 + payload_len);
        pb_string(rec, 1, name_, name_ ? std::strlen(name_) : 0);
        // field 2 (dst): omitted — proto3 default "" when absent.
        if (msg_type_name)
            pb_string(rec, 3, msg_type_name, std::strlen(msg_type_name));
        if (corr_id) pb_varint_field(rec, 4, corr_id);
        if (ts_ns)   pb_varint_field(rec, 5, ts_ns);
        uint64_t k = static_cast<uint64_t>(trace_kind_of(kind));
        if (k)       pb_varint_field(rec, 6, k);
        if (payload && payload_len)
            pb_bytes(rec, 7, payload, payload_len);

        TraceSubmitter::instance().submit(rec);
    }

    const char* node_name() const noexcept { return name_; }

private:
    // ── proto3 wire encoders (just the field types TraceRecord needs) ──
    // A varint LEB128, a length-delimited (string/bytes), and the
    // tag = (field_number << 3) | wire_type. Enough to serialize
    // TraceRecord by hand without nanopb callbacks in the hot path.
    static void pb_varint(std::string& out, uint64_t v) {
        while (v >= 0x80) {
            out.push_back(static_cast<char>((v & 0x7f) | 0x80));
            v >>= 7;
        }
        out.push_back(static_cast<char>(v));
    }
    static void pb_tag(std::string& out, uint32_t field, uint32_t wire) {
        pb_varint(out, (static_cast<uint64_t>(field) << 3) | wire);
    }
    static void pb_varint_field(std::string& out, uint32_t field, uint64_t v) {
        pb_tag(out, field, 0);          // wire type 0 = varint
        pb_varint(out, v);
    }
    static void pb_len_delim(std::string& out, uint32_t field,
                             const char* data, size_t len) {
        pb_tag(out, field, 2);          // wire type 2 = length-delimited
        pb_varint(out, len);
        out.append(data, len);
    }
    static void pb_string(std::string& out, uint32_t field,
                          const char* s, size_t len) {
        if (s && len) pb_len_delim(out, field, s, len);
    }
    static void pb_bytes(std::string& out, uint32_t field,
                         const uint8_t* b, size_t len) {
        pb_len_delim(out, field, reinterpret_cast<const char*>(b), len);
    }

    const char*                                    name_;
    std::atomic<bool>                              enabled_{false};
    std::atomic<bool>                              reporting_{false};
    std::chrono::steady_clock::time_point          start_tp_;

    // Per-msg-type filter (#355). Keys are stable string literals
    // emitted by RemoteCodec::msg_type_name() — std::string holds
    // a copy so the filter map doesn't outlive the literal's TU.
    mutable std::mutex                             filter_mu_;
    std::unordered_map<std::string, bool>          filter_;
};

// encode_for_trace + msg_type_name are defined in RemoteCodec.hh and
// included alongside Tracer.hh by callers (e.g. GenServer.hh includes
// both). Don't forward-declare them here — duplicate signatures
// confuse overload resolution at the call site.

// Mint a synthetic correlation id for local trace points (where no
// wire-level correlation_id exists). Remote/RPC paths use the real
// correlation_id from GwMessageHeader instead. Both share the
// 32-bit space; collisions don't matter because the trace stream
// distinguishes by (corr_id, msg_type) within the same node's events.
inline uint32_t next_trace_corr_id() noexcept {
    static std::atomic<uint32_t> ctr{1};
    return ctr.fetch_add(1, std::memory_order_relaxed);
}

// Process-wide registry. One Tracer per node-type-name; the supervisor
// (or test) flips Tracer::enable() to turn observation on/off without
// restart.
inline Tracer& tracer_for(const char* node_name) {
    static std::mutex mu;
    static std::unordered_map<std::string, Tracer*> table;
    // Boot-time master switch: THEIA_TRACE=1 enables every node's tracer
    // at creation, so a standalone run (no supervisor to push the
    // ConfigureTrace flip) still emits to stderr. Filter stays empty →
    // emit everything. Read once, cached. The supervisor path
    // (#361 trace_enable) still works on top of this when present.
    static const bool env_trace_on = [] {
        const char* v = std::getenv("THEIA_TRACE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    std::lock_guard<std::mutex> lk(mu);
    auto it = table.find(node_name);
    if (it == table.end()) {
        auto* t = new Tracer(node_name);
        if (env_trace_on) t->enable(true);
        table[node_name] = t;
        return *t;
    }
    return *it->second;
}

}  // namespace runtime
}  // namespace demo

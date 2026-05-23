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
#include <cstring>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

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

    // Hot path. Caller MUST have already checked enabled() — we don't
    // re-check, to make the cost model explicit at call sites.
    // payload may be nullptr / 0-len for events without typed data.
    // corr_id pairs Send/Recv across processes (for RPCs) or pairs
    // CallWait/CallResume locally; 0 means "no correlation".
    void emit(TraceEvent kind,
              const char* msg_type_name,
              uint32_t       corr_id,
              const uint8_t* payload, uint16_t payload_len) noexcept {
        auto now = std::chrono::steady_clock::now();
        auto ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - start_tp_).count();

        // Stack buffer for the hex encoding. 256B payload → 512B hex
        // + header overhead. Trace records >256B aren't emitted (the
        // payload is truncated at encoding time).
        char buf[1024];
        int n = std::snprintf(buf, sizeof(buf),
                              "TRC v1 %s %s msg=%s corr=%u ts=%lldms hex=",
                              trace_event_name(kind),
                              name_,
                              msg_type_name ? msg_type_name : "-",
                              corr_id,
                              static_cast<long long>(ts_ms));
        if (n < 0 || n >= static_cast<int>(sizeof(buf))) return;

        // Hex-encode payload inline. Trim if it wouldn't fit.
        uint16_t cap_pl = static_cast<uint16_t>(
            (sizeof(buf) - n - 2) / 2);  // -2 for trailing \n + NUL
        uint16_t out_pl = payload_len < cap_pl ? payload_len : cap_pl;
        static constexpr char hex[] = "0123456789abcdef";
        for (uint16_t i = 0; i < out_pl; ++i) {
            buf[n++] = hex[(payload[i] >> 4) & 0xf];
            buf[n++] = hex[payload[i] & 0xf];
        }
        buf[n++] = '\n';
        buf[n]   = '\0';
        std::fputs(buf, stderr);
    }

    const char* node_name() const noexcept { return name_; }

private:
    const char*                                    name_;
    std::atomic<bool>                              enabled_{false};
    std::chrono::steady_clock::time_point          start_tp_;
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
    std::lock_guard<std::mutex> lk(mu);
    auto it = table.find(node_name);
    if (it == table.end()) {
        auto* t = new Tracer(node_name);
        table[node_name] = t;
        return *t;
    }
    return *it->second;
}

}  // namespace runtime
}  // namespace demo

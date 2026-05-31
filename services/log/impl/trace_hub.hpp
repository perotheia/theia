// TraceHub — the process-global, mutex-guarded trace ring + subscriber
// registry shared by the two trace nodes:
//
//   TraceStreamPump (runnable)  calls submit(raw)   — ring + fan-out
//   TraceCtl        (atomic)    calls subscribe(...) — register an observer
//
// A process-global singleton (like process_logger()) so the two node threads
// reach the SAME hub without main.cc wiring (main.cc is generated). The hub
// NEVER decodes a TraceRecord — it stores + forwards raw proto-wire bytes
// verbatim (TraceRecord strings/bytes are nanopb pb_callback fields; only the
// observer decodes, via libprotobuf + JSON). All TIPC, no gRPC.
//
// See docs/tasks/TODO/composition-isolation-test.md.

#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "NodeRef.hh"        // theia::runtime::TipcClient
#include "RemoteCodec.hh"    // service_id for the fan-out frame
#include "TheiaMsgHeader.hh"

namespace ara::log {

// One registered observer. The hub holds a persistent SEQPACKET client to it
// (connect once on subscribe, reuse per fan-out). A failed send means the
// observer went away -> prune (connection-close demonitor, surfaced as
// send-failure since fan-out is outbound cast).
struct TraceSub {
    uint32_t tipc_type     = 0;
    uint32_t tipc_instance = 0;
    uint32_t kind_filter   = 0;   // TraceKind; 0 = all
    std::string node_filter;      // "" = all
    std::shared_ptr<::theia::runtime::TipcClient> client;
};

class TraceHub {
public:
    // service_id the observer's receiver registers for — the SAME djb2 the
    // producer/collector use for the TraceRecord type, so the fan-out frame
    // dispatches on the observer side.
    static constexpr uint16_t kRecordServiceId =
        ::theia::runtime::hash_msg_type_("system_services_log_TraceRecord");

    static TraceHub& instance() {
        static TraceHub h;
        return h;
    }

    void set_capacity(std::size_t n) {
        std::lock_guard<std::mutex> lk(mu_);
        capacity_ = n ? n : 1;
        while (ring_.size() > capacity_) ring_.pop_front();
    }

    // Runnable hot path: store raw record bytes in the ring + fan out to every
    // live subscriber. Prune subscribers whose send failed.
    void submit(std::string wire) {
        std::lock_guard<std::mutex> lk(mu_);
        ring_.push_back(std::move(wire));
        while (ring_.size() > capacity_) ring_.pop_front();
        const std::string& rec = ring_.back();
        auto it = subs_.begin();
        while (it != subs_.end()) {
            if (!send_locked(*it, rec)) {
                std::fprintf(stderr,
                    "[trace_hub] subscriber {0x%08x,%u} gone — pruning\n",
                    it->tipc_type, it->tipc_instance);
                it = subs_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Control path (atomic node): register an observer. Connects, spills the
    // ring backlog (adb logcat: history, then follow), then live records flow
    // via submit() fan-out. Returns false if the observer is unreachable.
    bool subscribe(uint32_t type, uint32_t instance,
                   uint32_t kind_filter, std::string node_filter) {
        auto client = std::make_shared<::theia::runtime::TipcClient>();
        if (!client->connect(type, instance)) {
            std::fprintf(stderr,
                "[trace_hub] subscribe: cannot reach observer {0x%08x,%u}\n",
                type, instance);
            return false;
        }
        TraceSub sub;
        sub.tipc_type     = type;
        sub.tipc_instance = instance;
        sub.kind_filter   = kind_filter;
        sub.node_filter   = std::move(node_filter);
        sub.client        = std::move(client);

        std::lock_guard<std::mutex> lk(mu_);
        for (const std::string& rec : ring_) {     // spill backlog
            if (!send_locked(sub, rec)) return false;  // died mid-spill
        }
        std::fprintf(stderr,
            "[trace_hub] subscriber {0x%08x,%u} attached "
            "(%zu backlog, %zu live)\n",
            type, instance, ring_.size(), subs_.size() + 1);
        subs_.push_back(std::move(sub));
        return true;
    }

private:
    TraceHub() = default;

    // Frame raw record bytes as a GEN_CAST and send to one subscriber.
    // Caller holds mu_. Filter is applied by the observer (it decodes); the
    // hub honours only a coarse kind/node filter when set, but since it does
    // NOT decode, filtering is best-effort and currently pass-through.
    bool send_locked(TraceSub& sub, const std::string& wire) {
        if (!sub.client || !sub.client->is_open()) return false;
        ::theia::runtime::TheiaMsgHeader hdr{};
        hdr.bus_type           = ::theia::runtime::kBusTypeRpc;
        hdr.msg_type           = ::theia::runtime::kMsgGenCast;
        hdr.proto_len          = static_cast<uint16_t>(wire.size());
        hdr.rpc.service_id     = kRecordServiceId;
        hdr.rpc.method_id      = 0;
        hdr.rpc.correlation_id = 0;
        return sub.client->send_frame(
            hdr, reinterpret_cast<const uint8_t*>(wire.data()),
            static_cast<uint16_t>(wire.size()));
    }

    std::mutex mu_;
    std::deque<std::string> ring_;
    std::size_t capacity_ = 4096;
    std::vector<TraceSub> subs_;
};

}  // namespace ara::log

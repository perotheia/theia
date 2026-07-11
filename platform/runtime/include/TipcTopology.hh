// TipcTopology — live TIPC service-presence discovery via the kernel topology
// service (TIPC_TOP_SRV). The answer to "is shwa:1 there?" WITHOUT a blind call
// + timeout: subscribe to a {type, instance-range} and the kernel pushes a
// PUBLISHED event the instant any matching service binds (cluster-wide) and a
// WITHDRAWN event the instant the last one unbinds. We keep a live presence set
// and fire a callback on every change, so a consumer (com) only ever fans out
// to instances that are actually up.
//
// How it works (Linux TIPC topology API, linux/tipc.h):
//   - connect a SOCK_SEQPACKET socket to the service {TIPC_TOP_SRV, TIPC_TOP_SRV}
//   - send one `struct tipc_subscr` per range of interest (timeout=FOREVER,
//     filter=TIPC_SUB_PORTS so we get an event per (type,instance) match —
//     PUBLISHED on bind, WITHDRAWN on unbind)
//   - recv `struct tipc_event`s; (found_lower==found_upper) is the instance.
// A dedicated thread blocks on recv; subscriptions can be added before or after
// start(). Thread-safe presence queries.
//
// Scope: subscribes at CLUSTER scope by default (the connect reaches the
// cluster-wide topology server), so com on central sees services on compute.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace theia {
namespace runtime {

// One observed (type, instance) presence change.
struct TopologyEvent {
    uint32_t type;
    uint32_t instance;
    bool     present;   // true = PUBLISHED (up), false = WITHDRAWN (last down)
};

class TipcTopology {
public:
    // Callback invoked on the topology thread for every presence change. Keep it
    // cheap + non-blocking (it runs in the recv loop); copy out what you need.
    using Callback = std::function<void(const TopologyEvent&)>;

    TipcTopology() = default;
    ~TipcTopology();

    TipcTopology(const TipcTopology&) = delete;
    TipcTopology& operator=(const TipcTopology&) = delete;

    // Register a range of interest: all instances [lower, upper] of `type`. For a
    // single-type fan-out (e.g. shwa across machines) use lower=0, upper=N (or
    // ~0u for "any instance"). Call before start(), or after — added live.
    // Returns false if the subscription couldn't be sent (no socket yet → queued
    // and sent at start()).
    bool subscribe(uint32_t type, uint32_t lower, uint32_t upper);

    // Start the topology thread (connects to TIPC_TOP_SRV, flushes queued
    // subscriptions, blocks on recv). Idempotent. Returns false if the connect
    // failed (no TIPC / topology server unreachable).
    bool start(Callback on_change);

    // Stop the thread + close the socket. Idempotent; called by the dtor.
    void stop();

    // Snapshot of all currently-present instances of `type` (sorted). Empty if
    // none are up. Lets a consumer enumerate "which machines have shwa right now".
    std::vector<uint32_t> instances_of(uint32_t type) const;

private:
    struct Range { uint32_t type, lower, upper; };

    bool connect_();                 // open + connect the SEQPACKET to TIPC_TOP_SRV
    bool send_subscr_(const Range&); // send one tipc_subscr on fd_
    void run_();                     // recv loop on the topology thread

    int                       fd_{-1};
    std::thread               thread_;
    std::atomic<bool>         running_{false};
    Callback                  cb_;

    mutable std::mutex        mu_;
    std::vector<Range>        ranges_;                  // subscriptions (for replay)
    std::set<uint64_t>        present_;                 // packed (type<<32 | instance)

    static uint64_t key_(uint32_t t, uint32_t i) {
        return (static_cast<uint64_t>(t) << 32) | i;
    }
};

}  // namespace runtime
}  // namespace theia

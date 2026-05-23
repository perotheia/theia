// supervisor/etcd_publisher.h — tee the supervisor's live state into etcd.
//
// Sits next to TipcPublisher in runtime.cpp. Every TIPC publish has
// an etcd-side counterpart:
//
//   emit_event   → /theia/events/<machine>/<ts>-<seq>     (no lease)
//   emit_health  → /theia/state/<machine>/health         (lease)
//   emit_snapshot→ /theia/state/<machine>/tree           (lease)
//                  + /theia/state/<machine>/child/<name> (lease)
//
// Lease semantics (10s grant, 3s KeepAlive):
//   Whatever supervisor wrote under /theia/state/<machine>/* drops
//   out of etcd within ≈10s of the supervisor dying. GUI sees the
//   machine vanish without needing a heartbeat watchdog of its own.
//   /theia/events/ writes are NOT leased — events persist past the
//   supervisor's lifetime as a forensic record.
//
// Opt-in: if etcd_endpoints is empty, open() returns true without
// connecting and every publish_*() is a no-op. Lets single-machine
// dev runs work without standing up etcd. Enable via:
//   - executor.yaml's optional `etcd_endpoints: ["host:2379"]`
//   - $THEIA_ETCD_ENDPOINTS env var
//   - --etcd-endpoints CLI flag (overrides both)
//
// Threading: publish_*() are called from the supervisor's main loop
// (single-threaded). The KeepAlive thread + Watcher (none yet) run
// inside etcd-cpp-apiv3. We hold a shared_ptr to KeepAlive so the
// lease stays alive until close().

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

// Forward-declare etcd-cpp-apiv3 types so this header doesn't pull
// in cpprestsdk / boost transitively. Implementations include the
// full headers in etcd_publisher.cpp.
namespace etcd {
class SyncClient;
class KeepAlive;
}

// C++14 nested-namespace style. Once the supervisor moves to C++17
// these can collapse to `namespace services::supervisor`.
namespace services { namespace supervisor {
class ChildState;
class HealthBeacon;
class SupervisionEvent;
class SystemInfo;
class TreeSnapshot;
}}

namespace supervisor {

class EtcdPublisher {
public:
    EtcdPublisher();
    ~EtcdPublisher();

    EtcdPublisher(const EtcdPublisher&)            = delete;
    EtcdPublisher& operator=(const EtcdPublisher&) = delete;

    // Open a connection + grant a 10s lease + start KeepAlive.
    //
    // Returns true even when endpoints is empty (no-op mode) — call
    // sites can ignore the result and treat all publish_*() as
    // safe-to-call always.
    //
    // On real failure (endpoints non-empty, grant or connect error)
    // returns false; subsequent publish_*() remain no-ops until a
    // future open() succeeds.
    bool open(const std::string& endpoints,
              const std::string& machine_name);

    // Cancel the KeepAlive thread, let the lease expire. Idempotent.
    void close();

    bool enabled() const { return enabled_.load(); }

    // ------- publish operations (all no-op when !enabled) ----------

    // /theia/state/<m>/tree — singleton, leased. Last writer wins.
    void publish_tree(const services::supervisor::TreeSnapshot& snap);

    // /theia/state/<m>/health — singleton, leased.
    void publish_health(const services::supervisor::HealthBeacon& hb);

    // /theia/state/<m>/child/<name> — one per child, leased. Caller
    // walks the tree and invokes per child.
    void publish_child(const services::supervisor::ChildState& ch);

    // /theia/events/<m>/<ts>_<seq> — appended forever (NOT leased).
    // Sequence makes ts-collision-safe inside a millisecond.
    void publish_event(const services::supervisor::SupervisionEvent& ev);

    // Remove the supervisor's own snapshot of a child — used when a
    // child is deleted via the OTP delete_child path so the tab
    // doesn't show ghost rows after the lease's TTL.
    void erase_child(const std::string& child_name);

private:
    std::string                       machine_name_;
    std::string                       state_prefix_;   // /theia/state/<m>/
    std::string                       events_prefix_;  // /theia/events/<m>/
    std::unique_ptr<etcd::SyncClient> client_;
    std::shared_ptr<etcd::KeepAlive>  keep_alive_;
    int64_t                           lease_id_{0};
    std::atomic<bool>                 enabled_{false};
    std::atomic<uint64_t>             event_seq_{0};
};

}  // namespace supervisor

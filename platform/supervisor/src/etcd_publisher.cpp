// etcd_publisher.cpp — implementation. See header for the design.

#include "supervisor/etcd_publisher.h"

// generated/ from artheia gen-proto
#include "ChildState.pb.h"
#include "HealthBeacon.pb.h"
#include "SupervisionEvent.pb.h"
#include "TreeSnapshot.pb.h"

// etcd-cpp-apiv3 — full headers only inside the .cpp.
#include <etcd/SyncClient.hpp>
#include <etcd/KeepAlive.hpp>
#include <etcd/Response.hpp>
#include <etcd/Value.hpp>

#include <chrono>
#include <cstdio>
#include <exception>
#include <string>

namespace supervisor {

namespace {

// One lease, refreshed every 3s, 10s TTL — matches the
// design in docs/tasks/BACKLOG/etcd-state-backbone.md.
constexpr int kLeaseTtlSec       = 10;
constexpr int kKeepAlivePeriodSec = 3;

void log_err(const char* fn, const std::string& msg) {
    std::fprintf(stderr, "etcd_publisher: %s: %s\n", fn, msg.c_str());
}

}  // namespace


EtcdPublisher::EtcdPublisher()  = default;
EtcdPublisher::~EtcdPublisher() { close(); }


bool EtcdPublisher::open(const std::string& endpoints,
                          const std::string& machine_name) {
    machine_name_   = machine_name;
    state_prefix_   = "/theia/state/"  + machine_name_ + "/";
    events_prefix_  = "/theia/events/" + machine_name_ + "/";

    if (endpoints.empty()) {
        std::fprintf(stderr,
            "etcd_publisher: disabled (no endpoints configured)\n");
        enabled_.store(false);
        return true;
    }

    try {
        client_ = std::make_unique<etcd::SyncClient>(endpoints);
    } catch (const std::exception& e) {
        log_err("connect", e.what());
        client_.reset();
        enabled_.store(false);
        return false;
    }

    // Grant a lease + immediately start a KeepAlive on it. The
    // KeepAlive object runs its own thread and renews every
    // ~ttl/2 by default — we explicitly set the refresh period.
    try {
        keep_alive_ = client_->leasekeepalive(kLeaseTtlSec);
        if (!keep_alive_) {
            log_err("leasekeepalive", "returned null");
            client_.reset();
            enabled_.store(false);
            return false;
        }
        lease_id_ = keep_alive_->Lease();
    } catch (const std::exception& e) {
        log_err("leasekeepalive", e.what());
        client_.reset();
        enabled_.store(false);
        return false;
    }

    enabled_.store(true);
    std::fprintf(stderr,
        "etcd_publisher: connected to %s, machine=%s, lease=%lld (ttl=%ds)\n",
        endpoints.c_str(),
        machine_name_.c_str(),
        static_cast<long long>(lease_id_),
        kLeaseTtlSec);
    (void)kKeepAlivePeriodSec;  // documented intent; KeepAlive picks its own.
    return true;
}


void EtcdPublisher::close() {
    if (!enabled_.exchange(false)) return;
    // Letting the KeepAlive shared_ptr go releases the renewal thread;
    // the lease expires within kLeaseTtlSec and all our leased keys
    // drop out of etcd. We don't explicitly revoke — letting the lease
    // expire naturally is the design: it gives the GUI a consistent
    // "machine vanished" semantic regardless of whether we exit
    // gracefully or get SIGKILLed.
    keep_alive_.reset();
    client_.reset();
    std::fprintf(stderr,
        "etcd_publisher: closed; lease %lld will expire within %ds\n",
        static_cast<long long>(lease_id_), kLeaseTtlSec);
}


// --------- publish helpers --------------------------------------------

namespace {

// Serialize-or-skip. Returns empty string on serialize failure (and
// logs); publish_*() skips the etcd call. We do NOT abort on a single
// failed serialization — the supervisor's main loop must keep running.
std::string serialize_or_skip(const google::protobuf::Message& m,
                               const char* what) {
    std::string out;
    if (!m.SerializeToString(&out)) {
        std::fprintf(stderr,
            "etcd_publisher: SerializeToString failed for %s\n", what);
        return {};
    }
    return out;
}

void put_leased(etcd::SyncClient& client,
                int64_t lease_id,
                const std::string& key,
                const std::string& value,
                const char* what) {
    try {
        auto r = client.set(key, value, lease_id);
        if (!r.is_ok()) {
            log_err(what,
                "set rc=" + std::to_string(r.error_code()) +
                " msg=" + r.error_message());
        }
    } catch (const std::exception& e) {
        log_err(what, e.what());
    }
}

void put_unleased(etcd::SyncClient& client,
                  const std::string& key,
                  const std::string& value,
                  const char* what) {
    try {
        auto r = client.put(key, value);
        if (!r.is_ok()) {
            log_err(what,
                "put rc=" + std::to_string(r.error_code()) +
                " msg=" + r.error_message());
        }
    } catch (const std::exception& e) {
        log_err(what, e.what());
    }
}

}  // namespace


void EtcdPublisher::publish_tree(
        const ::services::supervisor::TreeSnapshot& snap) {
    if (!enabled_.load()) return;
    auto v = serialize_or_skip(snap, "TreeSnapshot");
    if (v.empty()) return;
    put_leased(*client_, lease_id_, state_prefix_ + "tree", v, "publish_tree");
}


void EtcdPublisher::publish_health(
        const ::services::supervisor::HealthBeacon& hb) {
    if (!enabled_.load()) return;
    auto v = serialize_or_skip(hb, "HealthBeacon");
    if (v.empty()) return;
    put_leased(*client_, lease_id_, state_prefix_ + "health", v, "publish_health");
}


void EtcdPublisher::publish_child(
        const ::services::supervisor::ChildState& ch) {
    if (!enabled_.load()) return;
    if (ch.name().empty()) {
        std::fprintf(stderr,
            "etcd_publisher: publish_child skipping anonymous child\n");
        return;
    }
    auto v = serialize_or_skip(ch, "ChildState");
    if (v.empty()) return;
    put_leased(*client_, lease_id_,
               state_prefix_ + "child/" + ch.name(),
               v, "publish_child");
}


void EtcdPublisher::publish_event(
        const ::services::supervisor::SupervisionEvent& ev) {
    if (!enabled_.load()) return;
    auto v = serialize_or_skip(ev, "SupervisionEvent");
    if (v.empty()) return;
    // Key format: <ts_ms>-<seq> — sortable lexicographically AND
    // monotonic within a millisecond thanks to the seq counter.
    // Zero-pad ts to 16 digits + seq to 6 so etcd's lexical sort
    // matches numeric sort.
    char keybuf[64];
    uint64_t seq = event_seq_.fetch_add(1);
    std::snprintf(keybuf, sizeof(keybuf), "%016lld-%06llu",
                  static_cast<long long>(ev.timestamp_ms()),
                  static_cast<unsigned long long>(seq));
    put_unleased(*client_,
                 events_prefix_ + keybuf,
                 v, "publish_event");
}


void EtcdPublisher::erase_child(const std::string& child_name) {
    if (!enabled_.load()) return;
    if (child_name.empty()) return;
    try {
        auto r = client_->rm(state_prefix_ + "child/" + child_name);
        if (!r.is_ok() && r.error_code() != 100 /*not found*/) {
            log_err("erase_child",
                "rm rc=" + std::to_string(r.error_code()) +
                " msg=" + r.error_message());
        }
    } catch (const std::exception& e) {
        log_err("erase_child", e.what());
    }
}

}  // namespace supervisor

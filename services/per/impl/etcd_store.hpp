// EtcdStore — the SOLE etcd connection, wrapping etcd-cpp-apiv3's SyncClient.
//
// Isolates ALL etcd-cpp-apiv3 usage (and its heavy transitive link: grpc++,
// libprotobuf.so, cpprest) behind a small interface that mirrors per's config
// store ops. Only this TU includes etcd headers; the handlers talk to the
// abstract Store. Config VALUES are opaque binary-proto bytes — per stores +
// forwards them, never decodes the node's config type.
//
// Layout in etcd (path is STABLE, version-free — the digest rides in the value):
//   /theia/config/<target_node>   ->  <digest>\0<config-bytes>
// We pack digest + bytes into one value with a NUL separator so a single etcd
// KV holds both (etcd values are opaque byte strings). get/put split/join.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace system_services_per {

// One value read back from the store.
struct StoreValue {
    bool        found{false};
    std::string config;     // serialized <Node>Config (binary proto bytes)
    std::string digest;     // schema digest the bytes are encoded under
    int64_t     mod_rev{0}; // etcd revision (CAS / staleness)
};

// A watch event delivered to the watch callback: the new value + (when
// available) the previous one, so the consumer can compute a typed diff.
struct WatchEvent {
    std::string target_node;   // the <node> the changed key belongs to
    StoreValue  cur;           // new value
    StoreValue  prev;          // previous value (prev.found=false on first put)
};

// The abstract store the handlers use — lets the in-memory store and the etcd
// store share one call surface (and keeps the etcd link out of the handler TU).
class Store {
public:
    virtual ~Store() = default;
    virtual StoreValue get(const std::string& target_node) = 0;
    // Put with optional CAS (expect_rev != 0 must match current). Returns the
    // new mod_rev on success, or 0 on a CAS conflict.
    virtual int64_t put(const std::string& target_node,
                        const std::string& config, const std::string& digest,
                        int64_t expect_rev) = 0;
    // Register a watch over the whole /theia/config/ prefix; cb fires on each
    // change with prev+cur. One watch covers all targets (the cb filters).
    virtual void watch_prefix(std::function<void(const WatchEvent&)> cb) = 0;
    // True if this store delivers changes via watch_prefix (etcd). When true,
    // a Put's fan-out comes back through the watch cb, so the handler must NOT
    // also fan out inline (avoids a double push). False for in-memory.
    virtual bool is_watched() const = 0;
    // Every stored config: (target_node, value). For MigrateBulk's keyspace
    // walk. The list is a snapshot (etcd range read / map copy); the caller
    // re-puts each migrated value with CAS on its mod_rev.
    virtual std::vector<std::pair<std::string, StoreValue>> scan() = 0;
};

// Build the etcd-backed store (connects to `endpoint`, e.g. "127.0.0.1:2379").
// Returns nullptr if the client can't be constructed. Defined in etcd_store.cc
// (the only TU that pulls in etcd-cpp-apiv3).
std::unique_ptr<Store> make_etcd_store(const std::string& endpoint);

// Build the in-memory store — the etcd-free fallback (no etcd reachable) and
// the default for tests. Header-only impl below so a handler TU that doesn't
// link :per_etcd still gets it. watch_prefix is a no-op (in-memory has no async
// watch source; the handler fans out on Put directly).
std::unique_ptr<Store> make_memory_store();

// Process-shared store handle. PerClient::init owns the store (it set up the
// watch) and publishes the raw pointer here; PerManager reads it for MigrateBulk
// (which walks the keyspace). Both nodes are in one process; the store outlives
// both for the process lifetime. nullptr until PerClient::init runs.
void set_shared_store(Store* s);
Store* shared_store();

}  // namespace system_services_per

// State struct for PerClient — APP-OWNED, WRITE-ONCE.
//
// Holds the config gatekeeper's in-process state. The CONFIG STORE is now an
// abstract Store (etcd_store.hpp): etcd-backed when etcd is reachable, else the
// in-memory fallback. The handlers talk to the Store interface, so the wire
// surface + the ConfigUpdated cast path are store-agnostic. Subscriptions
// (target -> who to push to) stay node-local here.

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "etcd_store.hpp"   // Store / StoreValue / make_*_store

namespace ara::per {

// A watch subscription: who to cast ConfigUpdated to (by node name → resolved
// to a TIPC addr at push time) and which schema to deliver in.
struct Subscription {
    std::string subscriber_node;
    std::string want_digest;
    // The subscriber's TIPC INSTANCE (from WatchConfigReq.subscriber_instance).
    // per casts ConfigUpdated to (resolved_type, subscriber_instance) so a
    // per-INSTANCE key (<component>/<instance>) notifies the EXACT clone. Also the
    // instance is authoritatively parsed from the changed KEY at push time — this
    // records what the watcher declared so a later fan-out can match it.
    uint32_t    subscriber_instance = 0;
};

struct PerClientState {
    // The config store — etcd-backed or in-memory (chosen in init()).
    std::unique_ptr<Store> store;
    // target_node -> subscribers watching it.
    std::unordered_map<std::string, std::vector<Subscription>> watches;
};

}  // namespace ara::per

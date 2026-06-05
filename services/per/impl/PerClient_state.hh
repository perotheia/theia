// State struct for PerClient — APP-OWNED, WRITE-ONCE.
//
// Holds the config gatekeeper's in-process state. The CONFIG STORE is an
// in-memory map FOR NOW (proves the Get/Put/Watch + ConfigUpdated-cast path
// end-to-end before the etcd-cpp-apiv3 link goes in — see
// docs/tasks/TODO/services-db-state-gatekeeper.md). Swapping the store for the
// real etcd client touches only this struct + the handler bodies; the wire
// surface and the cast path stay put.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace system_services_per {

// One stored config value: the serialized node-config bytes + its schema
// digest + a monotonically increasing revision (stand-in for etcd's mod_rev).
struct StoredConfig {
    std::string config;     // serialized <Node>Config (binary proto bytes)
    std::string digest;     // schema digest the bytes are encoded under
    uint64_t    mod_rev{0}; // bumped on each Put (etcd revision analogue)
};

// A watch subscription: who to cast ConfigUpdated to (by node name → resolved
// to a TIPC addr at push time) and which schema to deliver in.
struct Subscription {
    std::string subscriber_node;
    std::string want_digest;
};

struct PerClientState {
    // target_node -> current config value.
    std::unordered_map<std::string, StoredConfig> store;
    // target_node -> subscribers watching it.
    std::unordered_map<std::string, std::vector<Subscription>> watches;
    // global revision counter (per-store, not per-key — simple + sufficient).
    uint64_t next_rev = 1;
};

}  // namespace system_services_per

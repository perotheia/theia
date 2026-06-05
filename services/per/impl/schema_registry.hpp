// SchemaRegistry — the process-global, mutex-guarded config-schema registry.
//
// Shared by the two per nodes (like services/log's TraceHub is shared by
// TraceCtl + TraceStreamPump): PerManager WRITES it (RegisterSchema /
// ListSchemas) and PerClient READS it (migration-on-read: is the stored digest
// a known schema for this config_type?). A process-global singleton with a
// short mutex, so either node thread can touch it safely.
//
// Keyed by config_type (the node-config message FQN, e.g. "SupervisorConfig"),
// value = the SET of registered schema digests for that type. The registry is
// the authority for "which schema versions exist"; the actual stored config
// values + their digests live in the store (PerClientState / etcd). Migration
// transforms (digest A -> digest B) are a later layer; the registry just tracks
// membership for now.

#pragma once

#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace system_services_per {

struct SchemaEntry {
    std::string config_type;
    std::string digest;
};

class SchemaRegistry {
public:
    static SchemaRegistry& instance() {
        static SchemaRegistry r;
        return r;
    }

    // Register a (config_type, digest). Idempotent — re-registering the same
    // pair is a no-op. Returns false only on an empty config_type/digest.
    bool register_schema(const std::string& config_type,
                         const std::string& digest) {
        if (config_type.empty() || digest.empty()) return false;
        std::lock_guard<std::mutex> lk(mu_);
        types_[config_type].insert(digest);
        return true;
    }

    // Is this digest a known schema for config_type?
    bool known(const std::string& config_type, const std::string& digest) const {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mu_));
        auto it = types_.find(config_type);
        return it != types_.end() && it->second.count(digest) != 0;
    }

    // All registered (config_type, digest) pairs, optionally filtered by
    // config_type ("" = all). Returned as a flat list for the ListSchemas wire.
    std::vector<SchemaEntry> list(const std::string& config_type = "") const {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mu_));
        std::vector<SchemaEntry> out;
        for (const auto& kv : types_) {
            if (!config_type.empty() && kv.first != config_type) continue;
            for (const auto& d : kv.second) out.push_back({kv.first, d});
        }
        return out;
    }

private:
    SchemaRegistry() = default;
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::set<std::string>> types_;
};

}  // namespace system_services_per

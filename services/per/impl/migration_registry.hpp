// MigrationRegistry — the process-global config-migration transform registry.
//
// A transform reshapes a config VALUE (opaque binary-proto bytes) from one
// schema DIGEST to another: fn(bytes-at-from) -> bytes-at-to. Transforms are
// registered at static-init (services/per/migrations/<type>/<from>_to_<to>.cc,
// same registrar pattern as platform/runtime/trace/trace_decoder_protos.cc).
//
// Digests are GLOBALLY UNIQUE per (config_type, version), so a transform is
// keyed by (from_digest -> to_digest) ALONE — config_type is implicit, which
// keeps per config-type-agnostic (it never needs to know what a value's proto
// type is, only how to walk it forward). Migrations CHAIN: a request for
// v1 -> v3 with edges v1->v2 and v2->v3 registered runs both in order. The
// chain is found by BFS over the registered edges, so the migration author only
// declares adjacent steps.
//
// Used by PerClient.GetConfig (lazy migration on read): if a stored value's
// digest != the caller's want_digest, migrate(stored_digest, want_digest, bytes)
// reshapes it before returning. A returned-empty optional means "no path" — the
// handler then returns the value verbatim (best-effort) and logs.

#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ara::per {

// One transform step: reshape config bytes from one digest to the next.
using Transform = std::function<std::string(const std::string& bytes)>;

class MigrationRegistry {
public:
    static MigrationRegistry& instance() {
        static MigrationRegistry r;
        return r;
    }

    // Register a single adjacent edge from_digest -> to_digest. Called at
    // static-init by the migration TUs.
    void add(const std::string& from_digest, const std::string& to_digest,
             Transform fn) {
        std::lock_guard<std::mutex> lk(mu_);
        edges_[from_digest].push_back({to_digest, std::move(fn)});
    }

    // Migrate `bytes` from `from_digest` to `to_digest` by walking the
    // registered edges (BFS). Returns the reshaped bytes, or nullopt if no path
    // exists. A same-digest request is a no-op (returns the bytes unchanged).
    std::optional<std::string> migrate(const std::string& from_digest,
                                       const std::string& to_digest,
                                       const std::string& bytes) const {
        if (from_digest == to_digest) return bytes;
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mu_));
        // BFS for the shortest chain of digests from -> to; record the
        // predecessor edge so we can replay the transforms in order.
        std::unordered_map<std::string, std::pair<std::string, const Transform*>>
            prev;                           // node -> (from, edge-fn)
        std::unordered_set<std::string> seen{from_digest};
        std::queue<std::string> q;
        q.push(from_digest);
        bool found = false;
        while (!q.empty() && !found) {
            const std::string cur = q.front(); q.pop();
            auto it = edges_.find(cur);
            if (it == edges_.end()) continue;
            for (const auto& e : it->second) {
                if (seen.count(e.to)) continue;
                seen.insert(e.to);
                prev[e.to] = {cur, &e.fn};
                if (e.to == to_digest) { found = true; break; }
                q.push(e.to);
            }
        }
        if (!found) return std::nullopt;
        // Reconstruct the path from->to, then apply transforms front-to-back.
        std::vector<const Transform*> chain;
        for (std::string at = to_digest; at != from_digest; ) {
            const auto& p = prev.at(at);
            chain.push_back(p.second);
            at = p.first;
        }
        std::string out = bytes;
        for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit)
            out = (**rit)(out);
        return out;
    }

private:
    MigrationRegistry() = default;
    struct Edge { std::string to; Transform fn; };
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::vector<Edge>> edges_;
};

// Convenience registrar for the migration TUs: a file-scope static of this type
// registers its edge at program start.
struct MigrationRegistrar {
    MigrationRegistrar(const std::string& from, const std::string& to,
                       Transform fn) {
        MigrationRegistry::instance().add(from, to, std::move(fn));
    }
};

}  // namespace ara::per

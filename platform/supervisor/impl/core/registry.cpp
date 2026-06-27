// Manifest + Registry implementation. See registry.h.

#include "registry.h"

#include "Logger.hh"   // platform/runtime — process_logger()

#include <stdexcept>
#include <string>

namespace supervisor {

// ---- Manifest -------------------------------------------------------------

// load_manifest (spec.cpp) opens the JSON and throws std::runtime_error on a
// missing or malformed file. We let that propagate: a supervisor with no
// manifest cannot supervise anything, so process startup aborts.
Manifest::Manifest(const std::string& path)
    : root_(load_manifest(path, &machine_name_)) {
    if (!root_) {
        // load_manifest throws rather than returning null, so this is defensive
        // — but make the contract explicit.
        throw std::runtime_error("Manifest: load_manifest returned null for " +
                                 path);
    }
}

// ---- Registry -------------------------------------------------------------

namespace {

// Parse the manifest's TIPC strings ("0x80010001", "0") into a numeric address.
// ok=false on a malformed address — the same guard the old resolver had.
ResolvedAddr parse_addr(const NodeInfo& ni) {
    ResolvedAddr a;
    try {
        a.type     = static_cast<uint32_t>(std::stoul(ni.tipc_type, nullptr, 0));
        a.instance = static_cast<uint32_t>(std::stoul(ni.tipc_instance, nullptr, 0));
        a.ok       = true;
    } catch (...) {
        ::theia::runtime::process_logger().warn(
            "registry: bad tipc addr for node '" + ni.name + "' (type='" +
            ni.tipc_type + "' instance='" + ni.tipc_instance + "')");
        a.ok = false;
    }
    return a;
}

}  // namespace

Registry::Registry(const Node& root) {
    // Depth-first over the supervision tree, indexing every worker's NodeInfo.
    // Workers map to their FIRST reporting node; each reporting node maps to its
    // own address. (Hot-added children have no NodeInfo, so they never appear —
    // exactly why this index is safe to freeze at load time.)
    std::vector<const Node*> stack{&root};
    while (!stack.empty()) {
        const Node* n = stack.back();
        stack.pop_back();
        if (n->is_worker()) {
            const WorkerNode& w = n->worker;
            bool worker_mapped = false;
            for (const auto& ni : w.nodes) {
                if (!ni.reporting) continue;       // non-reporting: not a target
                ResolvedAddr a = parse_addr(ni);
                // node-type name → its own address.
                by_node_.emplace(ni.name, a);
                // worker name → its FIRST reporting node (coarse "trace this
                // process" intent). Only the first wins.
                if (!worker_mapped) {
                    by_worker_.emplace(w.name, a);
                    worker_mapped = true;
                }
            }
        } else {
            for (const auto& c : n->sup.children) stack.push_back(c.get());
        }
    }
}

ResolvedAddr Registry::resolve(const std::string& name) const {
    // Worker name takes precedence (matches the old resolve order: try worker
    // first, then node-type).
    if (auto it = by_worker_.find(name); it != by_worker_.end()) {
        return ResolvedAddr{it->second.type, it->second.instance, it->second.ok};
    }
    if (auto it = by_node_.find(name); it != by_node_.end()) {
        return ResolvedAddr{it->second.type, it->second.instance, it->second.ok};
    }
    return ResolvedAddr{};  // unknown / non-reporting → ok=false
}

}  // namespace supervisor

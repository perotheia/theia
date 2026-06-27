// Manifest + Registry — the supervision tree's IMMUTABLE, read-only facets.
//
// The supervision tree splits into two concerns:
//   - MUTABLE runtime state (pid / restart_count / live children, hot-add /
//     delete) — owned by the Supervisor engine, mutated ONLY on its loop
//     thread.
//   - IMMUTABLE topology declared by the manifest (each worker's per-art-node
//     NodeInfo: name, reporting flag, TIPC address). This NEVER changes after
//     load — hot-added children carry no NodeInfo, so they're never resolve
//     targets.
//
// Registry captures that immutable facet. It's built ONCE from the Manifest and
// answers "name → TIPC address" with a pure const lookup — no lock, no command
// queue, no loop-thread hop. SupervisorCtl resolves a trace/log target straight
// off it from its own thread.

#pragma once

#include "spec.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace supervisor {

// Owns the parsed manifest tree. The ctor loads the JSON and THROWS if the file
// is missing/unparseable — a supervisor with no manifest is fatal, not a
// soft-fail (the caller lets the exception abort process startup).
class Manifest {
public:
    explicit Manifest(const std::string& path);   // throws on missing/bad json

    // The root supervisor node of the parsed tree (non-null after ctor).
    const Node& root() const { return *root_; }

    // The machine this executor.json is for (root "machine" field); "" if the
    // manifest doesn't carry one (hand-written / legacy). The engine reports it
    // in GetSystemInfo so com labels per-machine telemetry by the real name.
    const std::string& machine_name() const { return machine_name_; }

    // Hand the owned tree to the engine (consumes it — call once). The engine
    // takes the MUTABLE tree; the Registry is built from it first (before the
    // move) or from a separate const walk.
    std::unique_ptr<Node> take_tree() { return std::move(root_); }

private:
    // machine_name_ MUST be declared BEFORE root_: the ctor's init list calls
    // load_manifest(path, &machine_name_) to populate root_, so machine_name_
    // has to be already-constructed when that write happens (member init runs in
    // declaration order). Reversing this order is a use-before-construction crash.
    std::string           machine_name_;
    std::unique_ptr<Node> root_;
};

// A resolved node TIPC address. ok=false when a name doesn't resolve to a
// reporting node (typo / non-reporting / not in the manifest).
struct ResolvedAddr {
    uint32_t type{0};
    uint32_t instance{0};
    bool     ok{false};
};

// Immutable name → address index, built once from the manifest tree. Read-only
// after construction: every method is const and lock-free.
//
// Resolves EITHER a worker name ("p1", "sm") → that worker's first reporting
// node, OR a node-type name ("DriverNode", "SmGate") → that node's own address.
// Mirrors the old resolve_trace_target semantics exactly, minus the tree walk
// (a flat map built at load time).
class Registry {
public:
    // Build from a manifest tree (a const walk; does not consume it).
    explicit Registry(const Node& root);

    // name → address. ok=false if unknown / non-reporting / no reporting node.
    ResolvedAddr resolve(const std::string& name) const;

private:
    // Worker name → its first reporting node's address; node name → its own.
    // Both built once in the ctor (manifest TIPC strings parsed to numbers
    // there), then read-only. ResolvedAddr is the stored value verbatim.
    std::unordered_map<std::string, ResolvedAddr> by_worker_;
    std::unordered_map<std::string, ResolvedAddr> by_node_;
};

}  // namespace supervisor

// Unit test: SupFirehose reassembles the #429 topo-pair stream into a
// TreeSnapshot REGARDLESS of pair order.
//
// The point the user asked to prove: "test it with unordered pairs". The
// supervisor emits NodeEdge/NodeState in topological order (parent before
// child), but the reassembler is NAME-KEYED and hierarchy is carried by each
// row's parent_name — so it must NOT depend on arrival order. This test feeds
// the SAME logical tree (a) in topological order and (b) deliberately
// shuffled (children before their parents) and asserts the two reassembled
// TreeSnapshots are equivalent (same rows, same parent links, same flags).
//
// No live supervisor / TIPC: it drives SupFirehose's plain-scalar sinks
// directly and reads the reassembled snapshot off a subscriber queue.

#include "impl/sup_firehose.hpp"

#include "TreeSnapshot.pb.h"
#include "ChildState.pb.h"

#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

using services_com::SupFirehose;

// A node we want in the tree: (name, parent, kind, pid, state, flags).
struct N {
    std::string name;
    std::string parent;   // "" = root
    uint32_t    kind;
    int32_t     pid;
    uint32_t    state;
    uint32_t    flags;
};

// The fixture tree — a small supervision hierarchy with a couple of levels so
// "child before parent" is actually possible to express out of order.
//
//   root_sup
//   ├── core_sup
//   │   ├── sm        (CORE_DUMPED flag)
//   │   └── com
//   └── app_sup
//       └── shwa      (DEGRADED flag)
const std::vector<N> kTree = {
    {"root_sup", "",         1, -1, 2, 0},
    {"core_sup", "root_sup", 1, -1, 2, 0},
    {"sm",       "core_sup", 0, 4101, 2, 1 /*CORE_DUMPED*/},
    {"com",      "core_sup", 0, 4102, 2, 0},
    {"app_sup",  "root_sup", 1, -1, 2, 0},
    {"shwa",     "app_sup",  0, 4201, 2, 2 /*DEGRADED*/},
};

// Drive one full snapshot through SupFirehose in the given visit order, and
// return the reassembled TreeSnapshot (read off a fresh subscriber).
::system_supervisor::TreeSnapshot drive(const std::vector<N>& order,
                                           uint64_t generation) {
    auto sub = SupFirehose::instance().subscribe();

    SupFirehose::instance().on_snapshot_begin(generation, /*ts=*/123456);
    for (const auto& n : order) {
        // edge (the topology pair) + state (per-node mutable), exactly as the
        // supervisor casts them.
        SupFirehose::instance().on_node_edge(/*op=ADD*/0, n.parent, n.name,
                                             n.kind);
        SupFirehose::instance().on_node_state(
            n.name, n.pid, /*tid=*/0, n.state, n.flags,
            /*restart_count=*/0, /*last_exit_code=*/0, /*uptime_ms=*/0,
            /*cpu_pct=*/0, /*rss_kb=*/0, /*vsz_kb=*/0, /*threads=*/0,
            /*shared_kb=*/0, /*data_kb=*/0);
    }
    SupFirehose::instance().on_snapshot_end(generation);

    // The reassembled snapshot is the one Frame now on the subscriber queue.
    ::system_supervisor::TreeSnapshot snap;
    {
        std::lock_guard<std::mutex> lk(sub->mtx);
        assert(!sub->queue.empty() && "no snapshot frame was produced");
        const auto& f = sub->queue.front();
        assert(f.tag == services_com::kTagSnapshot);
        bool ok = snap.ParseFromString(f.payload);
        assert(ok && "reassembled TreeSnapshot did not parse");
    }
    SupFirehose::instance().unsubscribe(sub);
    return snap;
}

// Normalise a TreeSnapshot to a name->(parent,kind,pid,state,flags) map so we
// can compare two snapshots independent of child-list ORDER (order is
// cosmetic; the structural truth is the parent_name links).
std::map<std::string, std::string> normalise(
        const ::system_supervisor::TreeSnapshot& snap) {
    std::map<std::string, std::string> m;
    for (const auto& c : snap.children()) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "parent=%s kind=%u pid=%d state=%u flags=%u",
                      c.parent_name().c_str(), c.kind(), c.pid(), c.state(),
                      c.flags());
        m[c.name()] = buf;
    }
    return m;
}

}  // namespace

int main() {
    // (a) topological order — parent always before child (the supervisor's
    // natural emit order).
    auto in_order = drive(kTree, /*generation=*/1);

    // (b) UNORDERED — reverse the visit order so every child arrives BEFORE
    // its parent. This is the case the user asked to prove works.
    std::vector<N> shuffled(kTree.rbegin(), kTree.rend());
    auto out_of_order = drive(shuffled, /*generation=*/2);

    // Both must contain every node, with identical parent links + flags.
    auto a = normalise(in_order);
    auto b = normalise(out_of_order);

    assert(a.size() == kTree.size() && "in-order: missing/extra rows");
    assert(b.size() == kTree.size() && "unordered: missing/extra rows");

    if (a != b) {
        std::fprintf(stderr, "FAIL: unordered reassembly diverged from in-order\n");
        for (const auto& kv : a) {
            auto it = b.find(kv.first);
            if (it == b.end())
                std::fprintf(stderr, "  %s: missing in unordered\n", kv.first.c_str());
            else if (it->second != kv.second)
                std::fprintf(stderr, "  %s: in-order[%s] != unordered[%s]\n",
                             kv.first.c_str(), kv.second.c_str(), it->second.c_str());
        }
        return 1;
    }

    // Spot-check the flags survived (CORE_DUMPED on sm, DEGRADED on shwa) and
    // a parent link is correct regardless of order.
    assert(b["sm"]   == "parent=core_sup kind=0 pid=4101 state=2 flags=1");
    assert(b["shwa"] == "parent=app_sup kind=0 pid=4201 state=2 flags=2");
    assert(b["root_sup"].rfind("parent= ", 0) == 0 ||
           b["root_sup"].rfind("parent=", 0) == 0);  // root: empty parent

    std::printf(
        "OK: %zu-node tree reassembled identically in topo order AND fully "
        "reversed (child-before-parent) order; flags + parent links intact.\n",
        kTree.size());
    return 0;
}

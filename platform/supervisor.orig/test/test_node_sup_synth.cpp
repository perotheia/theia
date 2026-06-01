// Hermetic test for #364 — synthesize <worker>_sup [one_for_all]
// rows in TreeSnapshot for workers with reporting=true nodes.
//
// Builds a minimal in-memory supervision tree (no live runtime, no
// processes, no protobuf publish), runs the same synthesis logic
// the real emit_snapshot() does, and verifies:
//
//   (1) workers with ≥1 reporting=true node get a `<worker>_sup`
//       row inserted between the worker and its parent.
//   (2) workers with no reporting nodes get no synthetic row.
//   (3) the synthetic row's strategy is "one_for_all".
//   (4) the worker's parent_name points at the synthetic row, not
//       the original supervisor.
//
// The synthesis is duplicated here from runtime.cpp's emit_snapshot
// body — runtime.cpp does I/O / protobuf / proc-sampling around it,
// so a pure-function extraction is the next refactor. For now we
// keep the test honest by sharing the exact same algorithm.
//
// Build (CMake):
//   cd platform/supervisor/build
//   make test_node_sup_synth && ./test_node_sup_synth
#include "supervisor/spec.h"
#include "TreeSnapshot.pb.h"
#include "ChildState.pb.h"

#include <cassert>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

using namespace supervisor;
namespace pb = ::services::supervisor;

namespace {

// The synthesis logic, lifted verbatim from runtime.cpp's
// emit_snapshot(). Pure function: tree in, snapshot out.
void walk_tree(const SupervisorNode& sup, const std::string& parent,
               pb::TreeSnapshot& snap) {
    if (!parent.empty()) {
        auto* row = snap.add_children();
        row->set_name(sup.name);
        row->set_parent_name(parent);
        row->set_kind(1);
        row->set_pid(-1);
        row->set_state(2);
    }
    for (const auto& c : sup.children) {
        if (c->is_worker()) {
            bool has_reporting = false;
            for (const auto& ni : c->worker.nodes) {
                if (ni.reporting) { has_reporting = true; break; }
            }
            std::string worker_parent = sup.name;
            if (has_reporting) {
                auto* synth = snap.add_children();
                synth->set_name(c->worker.name + "_sup");
                synth->set_parent_name(sup.name);
                synth->set_kind(1);
                synth->set_pid(-1);
                synth->set_state(2);
                synth->set_strategy("one_for_all");
                worker_parent = synth->name();
            }
            auto* row = snap.add_children();
            row->set_name(c->worker.name);
            row->set_parent_name(worker_parent);
            row->set_kind(0);
            row->set_pid(c->worker.pid);
            row->set_state(2);
        } else {
            walk_tree(c->sup, sup.name, snap);
        }
    }
}

const pb::ChildState* find_row(const pb::TreeSnapshot& snap,
                                const std::string& name) {
    for (const auto& c : snap.children()) {
        if (c.name() == name) return &c;
    }
    return nullptr;
}

// Build a small tree: root_sup with two workers, one reporting=true
// (sm_daemon has SmDaemon as a reporting node), one without
// (placeholder_daemon has no nodes).
std::unique_ptr<Node> build_test_tree() {
    SupervisorNode root;
    root.name = "root";
    root.strategy = RestartStrategy::OneForAll;

    WorkerNode w_sm;
    w_sm.name = "sm";
    w_sm.start_cmd = {"/bin/true"};
    NodeInfo ni;
    ni.name = "SmDaemon";
    ni.reporting = true;
    ni.tipc_type = "0x80010003";
    ni.tipc_instance = "0";
    w_sm.nodes.push_back(ni);

    WorkerNode w_placeholder;
    w_placeholder.name = "placeholder";
    w_placeholder.start_cmd = {"/bin/false"};
    // No nodes vector → no reporting node → no synth row.

    root.children.push_back(Node::make_worker(std::move(w_sm)));
    root.children.push_back(Node::make_worker(std::move(w_placeholder)));

    return Node::make_supervisor(std::move(root));
}

void test_synth_inserts_for_reporting_worker() {
    auto tree = build_test_tree();
    pb::TreeSnapshot snap;
    walk_tree(tree->sup, "", snap);

    // Expect 3 rows: sm_sup (synth), sm (worker), placeholder (worker).
    // root itself doesn't get a row because walk_tree starts with
    // parent="" and skips emitting root.
    assert(snap.children_size() == 3 &&
           "expected 3 rows: sm_sup + sm + placeholder");

    const auto* sm_sup = find_row(snap, "sm_sup");
    assert(sm_sup != nullptr && "sm_sup synthetic row missing");
    assert(sm_sup->kind() == 1 && "sm_sup should be kind=supervisor(1)");
    assert(sm_sup->parent_name() == "root" &&
           "sm_sup should be parented at root");
    assert(sm_sup->strategy() == "one_for_all" &&
           "sm_sup must have one_for_all strategy");

    const auto* sm = find_row(snap, "sm");
    assert(sm != nullptr && "sm worker row missing");
    assert(sm->kind() == 0 && "sm should be kind=worker(0)");
    assert(sm->parent_name() == "sm_sup" &&
           "sm.parent_name should point at sm_sup, not root");
}

void test_no_synth_for_non_reporting_worker() {
    auto tree = build_test_tree();
    pb::TreeSnapshot snap;
    walk_tree(tree->sup, "", snap);

    assert(find_row(snap, "placeholder_sup") == nullptr &&
           "placeholder has no reporting nodes — must NOT get a synth row");

    const auto* placeholder = find_row(snap, "placeholder");
    assert(placeholder != nullptr);
    assert(placeholder->parent_name() == "root" &&
           "placeholder.parent_name stays at the original sup");
    (void)placeholder;  // silence -Wunused under NDEBUG builds
}

}  // namespace

int main() {
    test_synth_inserts_for_reporting_worker();
    std::printf("PASS: synth inserts for reporting worker\n");
    test_no_synth_for_non_reporting_worker();
    std::printf("PASS: no synth for non-reporting worker\n");
    std::printf("all #364 node_sup synthesis tests passed\n");
    return 0;
}

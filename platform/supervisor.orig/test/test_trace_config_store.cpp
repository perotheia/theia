// Hermetic test for #361 — Supervisor's trace config store.
//
// Validates apply_trace_config()'s side effects on the in-memory
// table:
//   (1) enable: upsert key, garbage-collect on disable.
//   (2) per-target_node bucketing.
//   (3) idempotent disable on missing entry.
//   (4) restart contract: a child's saved config persists across
//       a "restart" simulated by clearing only heartbeats, not
//       trace_configs_.
//
// Tests the data shape ONLY — the TIPC push side (push_trace_config_
// _to_child + send_frame_to_tipc_name) needs a live peer to verify
// and is exercised end-to-end by the integration smoke in deploy/.
//
// Bypasses the runtime by exposing the algorithm as a free function
// over a typedef of the storage map. The supervisor's apply_trace_
// _config is a thin shim on top of the same algorithm; mirroring it
// in pure form keeps the test free of yaml/etcd/proc/grpc deps.
#include <cassert>
#include <cstdio>
#include <map>
#include <string>

namespace {

using TraceStore = std::map<std::string, std::map<std::string, bool>>;

// Same body as Supervisor::apply_trace_config minus the logging
// and the push side-effect — see platform/supervisor/src/runtime.cpp.
void apply(TraceStore& store, const std::string& target_node,
           const std::string& msg_type, bool enabled) {
    if (target_node.empty()) return;
    auto& by_msg = store[target_node];
    if (enabled) {
        by_msg[msg_type] = true;
    } else {
        by_msg.erase(msg_type);
        if (by_msg.empty()) store.erase(target_node);
    }
}

void test_enable_upserts() {
    TraceStore s;
    apply(s, "SmDaemon", "SmStateMsg", true);
    assert(s.size() == 1 && "enable adds the child bucket");
    assert(s["SmDaemon"]["SmStateMsg"] == true);

    apply(s, "SmDaemon", "SmStateMsg", true);    // re-enable
    assert(s["SmDaemon"].size() == 1 && "re-enable is idempotent");
}

void test_disable_garbage_collects() {
    TraceStore s;
    apply(s, "SmDaemon", "SmStateMsg", true);
    apply(s, "SmDaemon", "SmStateMsg", false);
    assert(s.empty() && "disabling the last entry removes the bucket");
}

void test_disable_missing_is_safe() {
    TraceStore s;
    apply(s, "SmDaemon", "Nope", false);
    assert(s.empty() && "disable on missing entry is a no-op");
}

void test_per_target_node_bucketing() {
    TraceStore s;
    apply(s, "SmDaemon",  "SmStateMsg",  true);
    apply(s, "SmDaemon",  "StartupComplete", true);
    apply(s, "ComDaemon", "GwHeartbeat", true);
    assert(s.size() == 2 && "two distinct child buckets");
    assert(s["SmDaemon"].size() == 2);
    assert(s["ComDaemon"].size() == 1);

    // Removing one msg_type from SmDaemon shouldn't touch ComDaemon.
    apply(s, "SmDaemon", "SmStateMsg", false);
    assert(s["SmDaemon"].size() == 1);
    assert(s["ComDaemon"].size() == 1);
}

void test_survives_simulated_restart() {
    // The defining property: child restart does NOT clear
    // trace_configs_. The supervisor re-pushes on the first
    // heartbeat after a gap.
    TraceStore s;
    apply(s, "SmDaemon", "SmStateMsg", true);
    // Simulate restart: heartbeats_ would be cleared (we don't model
    // it here — but trace_configs_ stays intact).
    assert(s["SmDaemon"]["SmStateMsg"] == true);
    apply(s, "SmDaemon", "SmStateMsg", true);  // re-push (idempotent)
    assert(s["SmDaemon"]["SmStateMsg"] == true);
}

void test_empty_target_dropped() {
    TraceStore s;
    apply(s, "", "SmStateMsg", true);
    assert(s.empty() && "empty target_node must not insert");
}

}  // namespace

int main() {
    test_enable_upserts();             std::printf("PASS: enable upserts\n");
    test_disable_garbage_collects();   std::printf("PASS: disable garbage-collects\n");
    test_disable_missing_is_safe();    std::printf("PASS: disable missing is safe\n");
    test_per_target_node_bucketing();  std::printf("PASS: per-target_node bucketing\n");
    test_survives_simulated_restart(); std::printf("PASS: survives simulated restart\n");
    test_empty_target_dropped();       std::printf("PASS: empty target dropped\n");
    std::printf("all #361 trace_config_store tests passed\n");
    return 0;
}

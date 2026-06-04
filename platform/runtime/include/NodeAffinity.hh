// Per-node CPU affinity + scheduler application.
//
// A rig pins a specific artheia NODE (its thread) to cores + a scheduler via
// NodeToCPUMapping; the supervisor serializes that into the child's
// THEIA_NODE_CFG env var, and the hosting process's gen-app main.cc calls
// apply_node_affinity() right after node.start() to apply it to that node's
// pthread. Memory limits are NOT here (RLIMIT is per-process, not per-thread —
// per-node memory needs cgroup v2; see docs/tasks/TODO/per-node-affinity-sched).
//
// THEIA_NODE_CFG grammar (|-separated per node, ;-separated fields):
//   <node>=cpu:<c>,<c>,...;sched:<policy>[:<prio>];nice:<n>;dl:<r>,<d>,<p>
//   policy ∈ other|batch|idle|fifo|rr|deadline.  Missing field = leave default.
// Example: "sm_daemon=cpu:2,3;sched:fifo:80|sm_gate=cpu:1"

#pragma once

#include <pthread.h>

namespace theia {
namespace runtime {

// Find `node`'s entry in a THEIA_NODE_CFG spec and apply its affinity +
// scheduler to `th`. No-op when spec is null/empty or has no entry for `node`.
// Soft-fails (logs via process_logger, never aborts) on EPERM etc. — rtprio
// needs CAP_SYS_NICE; affinity needs nothing. `th` of 0 (node not started) is a
// no-op.
void apply_node_affinity(pthread_t th, const char* node, const char* spec);

}  // namespace runtime
}  // namespace theia

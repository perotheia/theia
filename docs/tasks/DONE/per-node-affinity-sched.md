# Per-node CPU affinity + scheduler (rig → executor.json → env → main.cc)

> **DONE (2026-06-04).** Landed in `79b7d3b`. Full chain wired:
> `NodeToCPUMapping` → executor.json `nodes[]` (cpus/sched/sched_prio) →
> supervisor serializes `THEIA_NODE_CFG` into the child fork env
> (`spec.{h,cpp}` NodeInfo + `runtime.cpp`) → `main.cc` `apply_node_affinity`
> (`platform/runtime/NodeAffinity.{hh,cc}`: pthread_setaffinity_np +
> pthread_setschedparam). Verified live (counter thread `cls=RR rtprio=40`).
> nice/DEADLINE (need TID) and per-node memory (need cgroup v2) deferred as
> noted in the body.

## Context

`NodeToCPUMapping` (artheia/manifest/machine.py) is fully DEFINED — per-node CPU
set + scheduling policy/priority/nice + SCHED_DEADLINE params — but **nothing
consumes it**: supervisor.py doesn't emit it, executor.json `nodes[]` carry only
name/reporting/tipc, and no generated main.cc applies any affinity/scheduler to a
node thread. Meanwhile PROCESS-level affinity (ProcessToMachineMapping →
ChildSpec.shall_run_on) IS wired (the supervisor applies `sched_setaffinity` in
its fork, runtime.cpp ~490).

Goal: wire `NodeToCPUMapping` end-to-end so a rig pins a specific node's THREAD
to cores + a scheduler, applied by the hosting process's main.cc.

## Decision

**Scope now: per-node CPU affinity + scheduler only. Memory DEFERRED.**
RLIMIT is per-PROCESS, not per-thread, so a true per-node memory cap needs
cgroups v2 (its own task). Process-level memory could ship later via RLIMIT_AS in
the supervisor fork (alongside the existing affinity). See "Deferred" below.

## Flow

```
rig.py (node_to_cpu_mappings / executor.py)
   → supervisor.py  : resolve core-name refs → ids; attach to per-node NodeInfo
   → executor.json  : nodes[] entries gain cpu[]/sched/prio/nice/deadline
   → supervisor     : serialize per-node cfg into THEIA_NODE_CFG env on the child
   → main.cc        : after node.start(), apply pthread_setaffinity_np +
                      pthread_setschedparam to that node's thread
```

### Env encoding — `THEIA_NODE_CFG`

One env var, `|`-separated per node, `;`-separated fields, `,`-separated cpu list:

```
THEIA_NODE_CFG=sm_daemon=cpu:2,3;sched:fifo:80|sm_gate=cpu:1
```

- `cpu:<c>,<c>,...`  — affinity set (positive list; empty/absent = inherit)
- `sched:<policy>[:<prio>]` — policy ∈ other|batch|idle|fifo|rr|deadline; prio for
  fifo/rr (1..99). Absent = leave default.
- `nice:<n>` — for other/batch.
- `dl:<runtime>,<deadline>,<period>` (ns) — only for deadline.

Key = the node's kNodeName (the prototype name in `tdb ps` / executor.json
nodes[].name). main.cc looks up its own node by kNodeName.

## Implementation

### 1. runtime — expose the node thread + an apply helper

`GenServerBase` / `GenRunnable`: add `pthread_t native_handle()` returning
`thread_.native_handle()` (0 if not started). Both already hold the std::thread.

New `platform/runtime/NodeAffinity.{hh,cc}` (or fold into a small util):
```cpp
// Parse one node's cfg field-string and apply to a pthread.
struct NodeSchedCfg { std::vector<int> cpus; int policy=-1; int prio=0; int nice;
                      long dl_runtime=0, dl_deadline=0, dl_period=0; };
bool parse_node_cfg(const std::string& spec, const char* node, NodeSchedCfg& out);
void apply_node_cfg(pthread_t th, const NodeSchedCfg& c);  // setaffinity + setschedparam
```
SCHED_DEADLINE needs the raw `sched_setattr` syscall (no glibc wrapper) — guard
it; fifo/rr/other use `pthread_setschedparam`. Soft-fail + log on EPERM (rtprio
needs CAP_SYS_NICE) — don't abort the process.

### 2. supervisor — NodeInfo carries cfg + emits the env

- spec.h `NodeInfo`: add `cpus` (vector<int>) + sched fields (policy ordinal,
  prio, nice, dl_*). Parsed in spec.cpp load_worker from the nodes[] JSON.
- runtime.cpp: when building the child env (alongside THEIA_LOGGER /
  THEIA_LOG_LEVEL), serialize each worker's nodes' cfg into THEIA_NODE_CFG and
  setenv it in the fork. (Engine-side: the env map is already setenv'd per child.)

### 3. manifest (artheia) — emit nodes[] affinity

supervisor.py `_collect_nodes_for_fc` / `_collect_nodes_for_app`: for each
NodeInfo, look up a matching `rig.node_to_cpu_mappings` (by node + process), and
emit cpu ids (resolve core-NAME refs to ints like PTM via `_ids_from_refs`) +
sched into the executor.json nodes[] entry.

### 4. main.cc templates — apply after start()

Both main.cc.j2 + main.statem.cc.j2, in the per-node block right after
`.start()` / `.start_statem()`:
```cpp
::theia::runtime::apply_node_affinity({{n.snake}}.native_handle(),
                                      {{n.name}}::kNodeName,
                                      std::getenv("THEIA_NODE_CFG"));
```
(a single helper that parses THEIA_NODE_CFG, finds this node's entry, applies.)

## Deferred — per-node memory

RLIMIT_AS/RLIMIT_DATA are per-PROCESS; you can't cap one thread's memory with
setrlimit. True per-node memory isolation = cgroup v2 (move the node thread into
its own cgroup with memory.max). That's a separate task. A cheaper interim:
PROCESS-level RLIMIT_AS applied in the supervisor fork (one cap for the whole FC
process), authored as Process.mem_limit — ship that if per-node isn't required.

## Verification

1. Build runtime + a demo FC (Demo3WayP1, 3 nodes).
2. Rig/manual: set THEIA_NODE_CFG=counter=cpu:1;sched:rr:50 (a core that exists),
   run, check `taskset -p <tid>` / `chrt -p <tid>` for the counter node thread vs
   the others (driver/ticker unpinned). `ps -L -o tid,psr,cls,rtprio,comm`.
3. Soft-fail: run without CAP_SYS_NICE → fifo/rr apply fails EPERM, logged, node
   keeps running (no abort). Affinity (no cap needed) still applies.
4. executor emit: confirm a rig with a NodeToCPUMapping emits cpu/sched into the
   right nodes[] entry, and the supervisor sets THEIA_NODE_CFG on that child.

# Per-machine supervisor TIPC instance (distinct supervisors, tdb -i)

Two machines on `network_mode: host` share one TIPC namespace, so two supervisors
at the SAME tipc instance (0x80020001 inst 0) collide — `tdb ps` sees only one.
Give each machine's supervisor a distinct TIPC **instance** so they coexist and
`tdb -i <n>` can address each.

ARA framing: the Executor IS the AUTOSAR Execution Management concept; the Theia
supervisor *implements* the Executor spec. So a machine's executor = its
supervisor composition (+ its tipc instance).

## The 4 pieces (build incrementally, test each)

### Step 0 — shwa→compute + deploy from per-machine specs  (PREREQ, easy)
DemoSoftware (the squash) mis-binds shwa to central. CentralSoftware/Compute-
Software already partition correctly (shwa+p3 on compute). Make the deploy emit
the partitioned shape. Fix: redefine DemoSoftware (after the partition lists) as
an explicit 2(+admin)-machine spec built from _central_*/_compute_* apps, so
shwa's COMPONENT lands on compute too (not just its process).
Test: dist/manifest/compute/application.json includes shwa; compute.ipk packs it.

### Step (a) — ComputeSupervisor .art prototype, tipc instance=1
system/system.art: a Supervisor prototype with instance 1.
    node ComputeSupervisor prototype Supervisor { tipc instance 1 }
    cluster Platform { composition Supervisor sup; composition ComputeSupervisor compute_sup; ... }
The central supervisor stays instance 0; compute's is instance 1.
Test: artheia parse system.art resolves both; the prototype carries instance=1.

### Step (b) — supervisor reads its TIPC instance (not hardcoded 0)
platform/supervisor/main.cc binds SupervisorCtl/Worker at a fixed instance. Make
it read the instance from env (THEIA_SUPERVISOR_INSTANCE) or the node cfg, so
compute's supervisor binds 0x80020001 inst 1. run-supervisor.sh / executor.json
sets it per machine. TipcMux already supports (type, instance).
Test: compute container's supervisor logs `instance=1`; `tipc nametable` shows
both inst 0 and inst 1.

### Step (c) — artheia Executor field + executor= on SoftwareSpecification
SoftwareSpecification/Rig gains `executor` — the supervisor component (+ its
instance) that implements ARA EM for this machine. ComputeSoftware sets
`executor=<compute_sup with instance 1>`. Flows into execution.json so the
supervisor (step b) reads its instance from the manifest, not a raw env.
Test: compute/execution.json carries the executor instance=1; central=0.

### Step (d) — tdb -i / --instance
tdb gains `-i <n>` (or `-i 0,1`) to pick which supervisor (instance) to target.
tdb_client resolves SupervisorCtl at (0x80020001, instance=n). `tdb -i 1 ps`
talks to compute; `tdb -i 0,1 ps` shows both trees.
Test: `tdb -i 0 ps` = central tree, `tdb -i 1 ps` = compute tree (p1/p2/p3+shwa),
both distinct.

## Current state (what exists)
- TipcMux supports (type, instance) bind. ✓
- tdb_client uses a per-process SOURCE instance (_unique_instance) but a FIXED
  TARGET (SupervisorCtl 0x80020001). No -i. ← (d)
- supervisor main.cc binds a fixed instance (0). ← (b)
- SoftwareSpecification has NO executor field. ← (c)
- CentralSoftware/ComputeSoftware partition shwa correctly; DemoSoftware (squash)
  does not. ← (0)

## Status — DONE 2026-06-07 (steps 0, a, b, d; c via env)

`tdb -i 0,1 ps` renders BOTH machines' trees distinctly with both containers up:
  -i 0 (central) → sm/log/per/ucm + p1/p2
  -i 1 (compute) → shwa + p3
TIPC nametable shows 0x80020001 at instance 0 AND 1 (no collision); both
containers RestartCount=0.

- Step 0 ✓ shwa→compute (DemoSoftware from partitioned per-machine apps).
- Step a ✓ ComputeSupervisor .art prototype at tipc instance=1.
- Step b ✓ supervisor main.cc reads THEIA_SUPERVISOR_INSTANCE (default kTipcInstance);
  binds ctl/worker at that instance.
- Step d ✓ tdb -i/--instance: SupervisorClient targets an instance-overridden
  SupervisorCtl ref; `-i 0,1` runs the command per instance with a header.
- Step c ✓ DONE — the instance flows through the MANIFEST, not a compose env:
  artheia `Supervisor` dataclass (its TIPC instance) + `Rig/SoftwareSpecification
  .supervisor` (machine name → Supervisor; the term is `supervisor`, the
  supervisor IMPLEMENTS the ARA Executor — `executor` is the artheia node-tree
  term). DemoSoftware sets supervisor={compute: Supervisor(instance=1)}. Emits
  <machine>/execution.json.supervisor_instance; run-supervisor.sh reads it →
  THEIA_SUPERVISOR_INSTANCE. The THEIA_SUPERVISOR_INSTANCE=1 compose env is
  REMOVED. Verified: both containers log "supervisor TIPC instance=N (from
  manifest)" (central 0, compute 1); tdb -i 0,1 ps distinguishes them.

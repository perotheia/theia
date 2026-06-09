# statem testing with rf-theia — events + state observation

Author: claude+roman · 2026-06-09
Builds on: `testing_framework_v3.md` (Pair 1 Hybrid automata),
`rf-theia-v3-probe-augment.md` (probe as transport substrate),
the STATEM trace work (from_state/to_state, committed `54e9a6b`/`ffe1f8d`).

## Goal

Test a `gen_statem` FC **standalone** — drive its events with `artheia.probe`
and assert on BOTH the transitions AND the FSM's state, the way OTP's
`sys:get_state` / `dbg` expose `{StateName, Data}`. The demo `DemoFsm`
(IDLE→PROCESSING→DONE, with a 5s timeout) is the first SUT; the loop
generalizes to `sm` and every future statem FC.

The insight that unifies the 6 points: a statem node is **also a gen_server
node**. It receives messages on a gen_server interface; the probe already
speaks that wire. We only need (a) a test-side *sender* node to give the probe
an identity, and (b) the FSM's **state** (OTP "Data" term) carried in the trace
so the test can assert on it, not just on transition names.

## The OTP framing (what "state" means here)

OTP `gen_statem` carries `{StateName, Data}`:
- **StateName** — the discrete state. Already in the trace as
  `from_state`/`to_state` (the enum name: "IDLE", "PROCESSING"). DONE.
- **Data** — the term threaded through `handle_event(State, Msg, Data)`.
  This is the FSM's `data <Msg>` in the `.art` (`DemoFsmData`). NOT yet
  observable. Point #4 is: surface this `Data` snapshot in the trace,
  decoded to JSON, so a test can assert `Wait For State PROCESSING` AND
  `Assert State {counter: 3}`.

The user modifies `Data` in the handler impl on each transition; the
framework snapshots it into the trace at the transition emit site. No new
user-facing wiring — the same `h.data` GenStateM already threads.

## The six points, mapped to concrete work

### 1. FSM defined — DONE
`DemoFsm` + `DemoFsmGate` in `demo/system/demo/package.art`, deployed under
`Demo3WayP4` / p4. Exercises event-driven (DemoStart/Finish/Reset) and
timeout (PROCESSING→IDLE after 5s) transitions.

### 2. Interface to test the FSM standalone — MOSTLY DONE
The gate (`DemoFsmGate`, TIPC 0xd0010007) is the FSM's gen_server ingress:
it receives `DemoFsmIn` events and `post_event`s them into the statem
in-process (the statem node itself takes no wire messages — sm split
pattern). The probe casts events at the gate's TIPC address. **No FC change
needed** — the gate already IS the standalone test surface.

### 3. Test node with a `sender` to drive the probe — DONE
The probe needs a node *definition* carrying a `sender` port on `DemoFsmIn`
to bind a tester identity and cast events. This node is NOT in the demo
cluster (not deployed) — it exists only so `artheia.probe` can construct a
client from the `.art`. Pattern: a `tdb.art`-style client node.

  - `DemoFsmTester` lives IN the demo package (`demo/system/demo/
    package.art`, package `system.demo`) — NOT a separate `demo_test` tree.
    It's a sibling of DemoFsmGate/DemoFsmIn, so it needs no import/extern and
    resolves through the existing canonical `system/demo` symlink like every
    other demo node: `node DemoFsmTester { sender events provides DemoFsmIn }`
    (tipc 0xd0010101). Not in any composition → gen-app excludes it from the
    deployed FCs (verified: regenerating p4 leaves no DemoFsmTester ref).
  - `demo/test/fsm_drive.py`: loads `system/demo/package.art`,
    `ctx.probe("DemoFsmTester").start()` then
    `probe.cast("DemoFsmGate", "DemoStart"|"DemoFinish"|"DemoReset")` over
    ONE persistent connection → ORDERED delivery (fixes the raw-socket
    race where DemoFinish dropped). Proven live: IDLE→PROCESSING→DONE→IDLE,
    each STATEM record carrying from→to + data={visits, reason}.
  - This is the `probe_external()` / tester-identity case from
    `rf-theia-v3-probe-augment` §Pair 4. rf-theia (Step C) imports the same
    context + casts, quarantined to one probe-adapter module.

### 4. Observe FSM **data** (OTP Data term) in the trace — NEW (runtime + gen)
Carry the FSM's `Data` message in the STATEM trace `payload`, OTP-style.
Decision (roman): name it **data** to match OTP `gen_statem`
(`handle_event(State, Msg, Data)`) — it's the `{State, Data}` snapshot's
*Data* half, NOT an "event payload". `from_state`/`to_state` are the State;
this is the Data.

  - **GenStateM.hh**: at each transition emit site, if `tr.enabled()`,
    encode `h.data` (the `DataT` nanopb message) into a scratch buffer and
    pass it as the trace `payload` (the field is already there, currently
    always empty for STATEM). Needs the data message's `pb_msgdesc_t*` —
    pass it as a generated `state_descriptor()` static on the daemon
    (matches the protobuf-cxx-pattern memo: descriptor as an arg, never a
    trait). Skip when `DataT` is the empty-POD placeholder (no `data`
    declared).
  - **gen-statem template** (`Daemon.statem.hh.j2`): emit
    `static const pb_msgdesc_t* state_descriptor()` returning the data
    message's fields (or `nullptr` when no `data`).
  - **observer.py**: when `kind==STATEM` and `payload` non-empty, decode
    it via the FSM data message type → `data` dict on `TraceRec`
    (reuse `_decode_inner`, but keyed on the FSM's data type, not
    `msg_type`). Add `data: dict|None` to `TraceRec` + `to_dict`.
  - **gRPC TraceRecord** already has `payload`; no proto change. The
    consumer (rtdb/GUI) renders `data={...}` for STATEM rows with data.
  - **demo**: give `DemoFsmData` a field (e.g. `uint32 visits` +
    `string reason`) and set it in `DemoFsm::on_enter` so the slice has
    something to assert on.

### 5. rf Hybrid-automata DSL slice — drive events, assert transitions+state
First real demo-FSM scenario, test-first per v3. Reuses the existing
`flow_engine.py` + the new probe sender + the trace observer.

  ```robot
  Demo FSM Runs To Done
      Start State Machine    DemoFsm     gate=DemoFsmGate
      Emit Event             DemoStart
      Wait For State         PROCESSING  within=2s
      Assert Data            visits=1
      Emit Event             DemoFinish
      Wait For State         DONE        within=2s
      Emit Event             DemoReset
      Wait For State         IDLE        within=2s
      Verdict    pass
  ```
  - `Emit Event` → probe cast at the gate (point 3).
  - `Wait For State` (state name) / `Assert Data` (OTP Data term) →
    trace observer (point 6).

### 6. Trace-observer module in rf for `Wait For State` — NEW (rf runtime)
rf-theia gets a live STATEM observer that decodes from/to/state and feeds
the flow engine's reactive `Wait For State`. Two source options already in
the tree:
  - the gRPC TraceStream client (com :7710) — used by the existing
    `trace_egress_lib`; carries the new fields after the regen.
  - `artheia.observer.TraceObserver` over TIPC — the probe-native path.
Pick the gRPC path for rf (com is already the rf trace surface), wrap it as
`runtime/state_observer.py` publishing `state_transition` events
(`node, from, to, data_json`) onto the existing `EventBus`. `Wait For
State` subscribes reactively (no polling/sleep). `Assert Data` reads the
decoded `data` dict (OTP Data term).

## Implementation order (this session: doc + e2e slice)

**A. State-in-trace (runtime + gen + observer)** — point 4.
  1. `Daemon.statem.hh.j2`: emit `state_descriptor()`.
  2. `GenStateM.hh`: encode `h.data` into trace `payload` at the 4 emit
     sites (guarded on a non-null descriptor).
  3. `observer.py`: decode payload→`data` on STATEM records.
  4. `DemoFsmData` gets fields; `DemoFsm::on_enter` sets them. Regen p4,
     stage, smoke-test via `tdb logcat` (should show `data={...}`).

**B. Test-node sender + probe injection** — point 3.
  5. `demo/test/demo_fsm_tester.art` — sender node on `DemoFsmIn`.
  6. rf `runtime/probe_adapter.py` (per probe-augment §1) OR a focused
     `demo_fsm_lib.py` first: build the context, cast events at the gate.

**C. rf observer + keywords + scenario** — points 5, 6. DONE.
  7. `runtime/statem_observer.py` — wraps `artheia.observer.TraceObserver`
     (TIPC, the probe-native path — NOT gRPC/com; clients via .art + probe),
     filters STATEM records for one node, publishes `statem_transition`
     events (`node, event, from_state, to_state, data`) on the EventBus.
  8. `runtime/probe_adapter.py` — wraps the probe to cast events at the
     gate (ordered, one connection). Both quarantine the artheia import.
  9. `scenarios/demo/fsm/demo_fsm_lib.py` — a self-contained scenario lib
     (sm_gate idiom) with role-named keywords: `Start/Stop Demo Fsm Stack`
     (stage + run supervisor, enable STATEM trace via the tdb
     SupervisorClient, attach observer + probe), `Emit Fsm Event`,
     `Wait For Fsm State`, `Assert Fsm Data`.
  10. `scenarios/demo/fsm/demo_fsm.robot` — two tests, GREEN twice:
      the full IDLE→PROCESSING→DONE→IDLE walk asserting each transition +
      `reason`, and a monotonic-`visits` check across a second walk.
  - **Pre-existing codec bug fixed** (artheia `probe/codec.py`): two `Codec`
    instances each `mkdtemp`'d their OWN `_pb2` out dir, so once one compiled
    `system.supervisor` (caching `sys.modules['system']` at its dir), a second
    compiling `system.services.log` failed with `No module named
    'system.services'`. Made `_out` a process-shared singleton across all
    Codec instances → every sub-package lives under one `system`/`platform`
    namespace root. (Surfaced because the lib uses BOTH the supervisor client
    AND the log observer in one process.)

**D. Generalize to sm + rf reorg** — DONE.
  - The statem keywords are now GENERIC + live in the shared
    `TheiaTestLibrary`: `Start/Stop Statem Stack`, `Emit Statem Event`,
    `Wait For Statem State`, `Assert Statem Data`. Backed by
    `adapters/hybrid_automata.py` (`HybridAutomata`, parameterized by
    node/gate/tester/art) — the SAME keywords drive demo_fsm AND sm.
  - `SmTester` (sender on LifecycleIn, tipc 0x8001002D) added to sm's
    package.art (mirrors DemoFsmTester; not in any composition → gen-app
    excludes it). sm's FSM data is `SmStateMsg{state,ts_ns}` and on_enter
    already sets it, so the STATEM trace carries `data={state, ts_ns}`
    (state=1 STARTING, 2 RUNNING). On psp-retirement the supervisor no
    longer auto-drives sm (it sits at OFF) so the test drives the boot
    handshake itself.
  - Tests: `demo/test/demo_fsm.robot` (app test) + `services/sm/test/
    sm_fsm.robot` (service test) — both GREEN (also green run together as
    sequential suites, each owning the central stack).
  - **Ready-check is transport-based** (poll the gate's TIPC binding), NOT a
    log grep: sm logs via the structured Logger to its own file sink
    (/tmp/theia/sm.log), so a supervisor-stdout grep wouldn't see it; only
    raw-fprintf(stderr) nodes (demo_fsm) appear there. The gate-binding poll
    is uniform across FCs.
  - **rf reorg** (roman's convention): `scenarios/` holds selftest `.robot`
    only — the demo lib left it (robot → `demo/test/`, impl → adapter +
    keywords). Service/app tests live in `<fc>/test/`. The 10 `_selftest/
    *_lib.py` + 2 `services/log/*_lib.py` are kept as-is (deferred sweep).

  Future (still backlog): fold the v1 `T Sup`/`T Sig` keywords behind the
  role-named surface; sweep the remaining selftest libs into
  SelftestLibrary.py + adapters/selftest/.

## Boundary / constraints (carried)
- rf-theia may import `artheia.gen_server.probe` (a stable contract, per
  probe-augment §boundary); MUST NOT import `artheia.model`/`.generators`.
- Quarantine the probe import to one adapter file.
- Probe stale-co-binding retry (project-probe-connect-stale-bindings).
- Don't rewrite platform/runtime — the state-in-trace change is additive
  (payload was already there, always empty for STATEM).
- gen-app via the canonical `system/<pkg>` symlink path.

## Done — A, B, C all landed
The `demo_fsm.robot` scenario drives DemoStart/Finish/Reset through the
probe and asserts each transition AND the decoded data via `Wait For Fsm
State` + `Assert Fsm Data`, GREEN (twice) against a live supervisor (no com
needed — observer + probe are TIPC-direct). The data-in-trace + sender +
observer + adapter pieces are reusable for `sm` and every statem FC.

Remaining (Step D, backlog): re-run B/C against `sm` (StartupComplete,
OFF→…→RUNNING) and graduate the role-named keywords into the shared
TheiaTestLibrary, folding the v1 `T Sup`/`T Sig` keywords behind them.

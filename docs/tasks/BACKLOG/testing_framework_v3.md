# rf-theia v3 — Robot-surface DSL, Python-resident semantics

Author: claude+roman · 2026-05-24
Supersedes: `testing_framework_v2.md` on the **DSL surface** question.

## Three corrections that landed v3

1. **Artheia is the SUT — keep test harness OFF its toolchain.** v2's
   proposal to build a textX `.theia` grammar through artheia's
   pipeline entangles the test framework with the system-under-test
   parser. rf-theia reads artheia's stable *outputs* (`rig.json`,
   `netgraph.json`, manifest) but never imports `artheia.*`.

2. **There is no .theia DSL — Robot keywords ARE the DSL.** Robot's
   table-oriented surface is fine as long as the keywords name roles,
   not adapters. Robot alone cannot model hybrid automata / supervision
   graphs / distributed actors / temporal logic / topology — but those
   five capabilities sit in **paired Python modules** with thin Robot
   keyword surfaces on top. The Python carries semantics; the keywords
   are the DSL.

3. **No new rig YAML.** Artheia already emits a typed rig description
   (`rig.json`, `manifest/<machine>/...`). Pydantic loads those into a
   typed `Rig` context object in `Suite Setup`. There is no separate
   file format for the test author to learn.

## The five (Python, Robot) pairs

Each capability Robot can't model natively becomes a Python module
paired with a small role-named keyword family. Each pair is grown
only when a real theia test demands it.

| # | Capability | Python module | Robot keyword surface | First theia test that drives it |
| --- | --- | --- | --- | --- |
| 1 | Hybrid automata | `runtime/flow_engine.py` (TPT-derived) | `Start State Machine`, `Emit Event`, `Wait For State` | sm_daemon restart, 4-state FSM with timeout |
| 2 | Temporal logic | `runtime/monitors.py` | `Assert Eventually`, `Assert Always`, `Assert Never` | broadcast resumes within 5s of restart |
| 3 | Supervision graph | `runtime/supervision_view.py` | `Assert Healthy`, `Assert Restart Order` | rest_for_one restarts dependents in order |
| 4 | Distributed actors | `runtime/components.py` | `Run Component`, `Send`, `Receive On`, `Verdict` | gateway-tester + observer-tester concurrent |
| 5 | Topology | `runtime/topology.py` (reads artheia rig.json) | `Reachable`, `Route Exists`, `Connected To` | sm reaches com via tipc_cluster |

The order is the iteration order — pair 1 ships first against real
theia, then pair 2 follows, etc.

## What Suite Setup looks like

A scenario opens with one keyword that loads the rig:

```robot
*** Settings ***
Library          rf_theia.TheiaTestLibrary

Suite Setup       Load Rig    %{RIG_JSON=dist/manifest/demo3way/rig.json}
Suite Teardown    Tear Down Rig
```

`Load Rig` calls `runtime.load(path)`, which:

- reads artheia's emitted `rig.json`
- parses into a typed `Rig` Pydantic model
  (`runtime/rig_schema.py` — schema describes what artheia EMITS, not a
  new file)
- builds the in-memory topology graph
- opens adapters lazily — supervisor gRPC connects on first need,
  trace tails open on first need

The Robot author never sees the structure. The runtime sees a typed
`Rig` object with `.machines`, `.services`, `.supervision`,
`.netgraph`, available to every keyword.

If artheia changes its output format, only `rig_schema.py` updates.
That's the SUT/test boundary holding.

## The four-keyword scenario test

```robot
*** Settings ***
Library    rf_theia.TheiaTestLibrary

Suite Setup       Load Rig    %{RIG_JSON}
Suite Teardown    Tear Down Rig


*** Test Cases ***
SM Restart Recovers Within Budget
    [Tags]    hybrid-automata    live    priority-high

    Start State Machine    RestartChild    target=sm_daemon
    Emit Event             crash    on=sm_daemon
    Wait For State         Restarted    within=10s

    Verdict    pass
```

The scenario:

- doesn't connect to anything (`Load Rig` did)
- doesn't poll anything (`Wait For State` is reactive — runtime fires
  when the FSM enters the state)
- doesn't write control flow (no if/for/while)
- doesn't time anything by hand (`within=10s` is reactive timeout, not
  `sleep`)

## Iteration loop — test-first against real theia

This is the DSL emergence process. For each capability:

1. **Pick a real theia test scenario** that exercises the capability.
2. **Write the Robot scenario first**, inventing keywords as needed.
3. **Implement just enough Python** to make those keywords pass.
4. **If theia is missing something** (a trace event, a supervisor RPC,
   a service hook), that's the next theia task — the test is the
   spec.
5. **Run the test. Does it read like a model?** If yes, keep. If no,
   refactor the surface.
6. **Move to the next test.** Reuse / extend.

After ~5 tests the surface stabilizes. After ~10 it's the DSL. No
upfront DSL design.

## What happens to the v1 keyword surface

As each (Python, Robot) pair lands, the v1 keywords it subsumes get
hidden. They move from `@keyword` decorators on `TheiaTestLibrary` to
internal methods on the runtime adapters. The keyword catalog stays
focused on the role-named surface.

| v1 keyword | Hidden when… |
| --- | --- |
| `T Sup Connect` / `Disconnect` | Pair 1 lands — `Load Rig` owns connection lifecycle |
| `T Sup Restart Child` | Pair 1 lands — wrapped by `Emit Event crash` in the RestartChild flow |
| `T Sup Expect Child State` | Pair 1 lands — used internally by `Wait For State` |
| `T Sig Open Trace` / `Close Trace` | Pair 1 lands — `Load Rig` opens, runtime tails |
| `T Sig Expect Trace` | Pair 2 lands — replaced by `Assert Eventually trace.event(...)` |
| `T Sig Expect Order` / `Latency` | Pair 2 lands — temporal monitor internals |
| `Create Partition` / `Add Transition` / `Set Signal` / `Apply Ramp` | **Kept** — already declarative, TPT-shaped |

Phase-1 hermetic selftest stays green throughout — `Library Imports`,
`TPT Engine Wires`, `T Wait Works` don't depend on the v1 keywords
being public.

## Where Robot stops being the right surface

Robot stays right as long as the scenario reads as a sequence of
role-named keyword calls that NAME entities defined elsewhere. The
stop conditions:

1. Scenario length \> ~30 keywords → split or move logic to a runtime
   flow.
2. Imperative branches needed → two scenarios with different tags, or
   a runtime flow.
3. Raw temporal math (`Sleep N; Check X`) → use `Wait For State` or
   `Assert Eventually`.
4. Non-trivial assertion parsing → drop to pytest against
   `rf_theia.runtime` directly.

Same runtime is reachable from pytest, MCP, and standalone Python.
Robot is the productive default for ~90% of tests; pytest is the
escape hatch.

## Phased rollout — iteration, not waterfall

### Pair 1 — Hybrid automata (next up)

First real theia test: **SM Restart Recovers Within Budget.**

Concrete tasks (one task per landed thing, growth-driven):

1. `runtime/__init__.py` + `runtime/loader.py` skeleton — `Load Rig`
   reads artheia's `rig.json`; Pydantic schema models what artheia
   emits, no new format.
2. `runtime/flow_engine.py` — FSM runner over the vendored TPT
   engine, accepts flow definitions as Python objects (not YAML).
3. `runtime/flows/restart_child.py` — first flow, hand-written
   Python — the equivalent of an SCXML state machine for the
   restart-and-wait cycle.
4. Library keywords: `Load Rig`, `Tear Down Rig`,
   `Start State Machine`, `Emit Event`, `Wait For State`, `Verdict`.
5. The scenario `signal_flow/sm_restart_recovers.robot`.
6. Theia-side: if `sm_daemon` doesn't emit a trace event the test
   needs, file a theia task and add it. Test is the spec.

### Pair 2 — Temporal logic

Driven by the second test that needs `Assert Eventually` /
`Assert Always` / `Assert Never`. Probable shape:

```robot
Broadcast Resumes After Restart
    Restart Child          sm_daemon
    Assert Eventually      trace.event(send) on sm_daemon   within=5s
    Assert Always          service(sm).heartbeat_period < 100ms during=5s
```

Cuts when pair 1 is stable.

### Pair 3 — Supervision graph

Driven by the first scenario needing strategy-aware assertions
(rest_for_one, one_for_all, restart-limit escalation). Cuts when pair 2
is stable.

### Pair 4 — Distributed actors

Driven by the first scenario needing a tester component co-resident
with the SUT. Cuts when pair 3 is stable + HIL transport is wanted.

### Pair 5 — Topology

Driven by the first scenario needing to assert "service X is reachable
from service Y via bus Z". Lowest priority for SIL; rises for HIL.

## Decisions captured (v3)

- **No new grammar.** Robot keywords are the DSL surface.
- **No new rig file format.** Pydantic loads artheia's `rig.json` into a
  typed context. The schema describes artheia's OUTPUT, not a new file.
- **No artheia coupling.** rf-theia reads artheia's outputs via
  Pydantic, never imports `artheia.*`.
- **Five paired (Python, Robot) modules.** One capability per pair.
- **Test-first against real theia.** Missing SUT behavior is a theia
  task, not a test bypass.
- **v1 keywords get hidden, not deleted.** As role-named pairs land,
  the adapter-named v1 keywords move internal. Selftest stays green.
- **Robot has a graduation path.** When tests outgrow Robot, the same
  runtime is reachable from pytest / MCP / standalone Python.

## Memo

What kept changing across v1 → v2 → v3 was the **boundary** — what's
artheia's job, what's the runtime's, what's the test's. Each revision
pulled the boundary somewhere else:

- v1: everything in the keyword library → test author writes
  orchestration → spaghetti.
- v2: orchestration in a `.theia` DSL → entangles the SUT and the
  harness → wrong layer.
- v3: orchestration in Python runtime → Robot names entities →
  artheia's `rig.json` carries the topology → boundaries align with
  what each component already owns.

The lesson: when in doubt, ship the boundary, not the abstraction.
The DSL emerges from the test corpus, not from a design doc.
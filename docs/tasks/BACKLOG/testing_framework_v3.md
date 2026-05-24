# rf-theia v3 — Robot-surface DSL, Python-resident semantics

Author: claude+roman · 2026-05-24
Supersedes: `testing_framework_v2.md` on the **DSL surface** question.
The five-layer conceptual model (resources / supervision / flows /
actors / temporal) from v2 stays. The textX `.theia` grammar from v2
is dropped.

## Two corrections from v2

### 1. Artheia is the SUT — keep test harness OFF its toolchain

v2 proposed parsing a new `.theia` grammar through artheia's textX
pipeline. That entangles the system-under-test description language
with the testing framework. If the test framework discovers an artheia
bug, the framework needs to keep working; if the test framework's
parser breaks, artheia must not care. Hard boundary:

> Artheia describes the system. rf-theia describes tests of that
> system. They share no parser, no AST, no model code, no import.

The test framework MAY read artheia's emitted artifacts (the JSON
netgraph, the manifest, the rig.json) — those are stable outputs of
the SUT, not internal AST. But rf-theia never imports `artheia.model`,
`artheia.grammar`, or anything from inside that package.

### 2. There is no .theia DSL — Robot keywords ARE the DSL

Robot Framework is the surface. It has real constraints:

- table-oriented, one keyword per line, 4-space arg separation
- no block syntax, no nested expressions, no operator precedence
- arguments are strings (with limited type coercion)
- control flow is severely limited by design

These constraints turn out to be features when treated honestly. The
DSL design becomes: **how do we name keywords such that the scenario
reads like a model, even though it's a flat list of calls?**

Done well, this gives ~80% of what a textX DSL would give. Done badly,
it's the keyword spaghetti you correctly flagged earlier.

The strategy: **context-sensitive keyword families** where the
*scenario structure* — `Suite Setup`, `Suite Teardown`, ordering of
`Test Cases`, `[Setup]`/`[Teardown]` per case — encodes orchestration,
and **the runtime owns all reactive/temporal semantics**.


## The four-keyword scenario test

Concrete v3 scenario, end-to-end:

```robot
*** Settings ***
Library    rf_theia.TheiaTestLibrary

Suite Setup       Load Rig    rigs/demo.yaml
Suite Teardown    Tear Down Rig


*** Test Cases ***
SM Crash Recovers Within Budget
    [Tags]    supervision    signal-flow    live

    Start State Machine    RestartChild    target=sm_daemon
    Emit Event             crash    on=sm_daemon
    Wait For State         Restarted    within=10s

    Assert Healthy         sm_daemon
    Assert Eventually      service.state(gateway) == HEALTHY    within=5s
    Assert Never           supervision.state(compute_domain) == DEADLOCK
    Assert Always          service(sm).heartbeat_period < 100ms    \
                           while=flow(RestartChild).active

    Verdict    pass
```

What's gone vs v1:

- No `T Sup Connect` / `Disconnect` — rig load owns connection lifecycle
- No `T Sig Open Trace` / `Close Trace` — runtime tails per the rig file
- No polling loops — `Wait For State` is reactive, the runtime fires it
- No raw `T Sup Restart Child` — that's now an event injected INTO a
  state machine the runtime is running

What's added:

- **State-machine keywords** (`Start State Machine`, `Emit Event`,
  `Wait For State`) — TPT/SCXML semantics surfaced as call-chain
- **Predicate keywords** (`Assert Eventually`, `Assert Never`,
  `Assert Always`) — temporal logic surfaced as call-chain
- **Verdict keyword** — TTCN-3-style outcome

The Robot file looks like an imperative call list. The runtime sees a
declarative model.


## The four-pillar runtime architecture

```
┌────────────────────────────────────────────────────────────────┐
│  Robot scenario (.robot)                                       │
│  — flat keyword list — translates 1:1 to runtime events        │
└──────┬─────────────────────────────────────────────────────────┘
       │ keyword calls
       ▼
┌────────────────────────────────────────────────────────────────┐
│  rf_theia.TheiaTestLibrary  (the thin veneer)                  │
│  — every keyword forwards to runtime.dispatch(...)             │
│  — no business logic in the library                            │
└──────┬─────────────────────────────────────────────────────────┘
       │
       ▼
┌────────────────────────────────────────────────────────────────┐
│  rf_theia.runtime  (where the actual semantics live)           │
│                                                                │
│  ┌─────────────┐  ┌─────────────┐  ┌────────────────────────┐  │
│  │   Topology  │  │ Supervision │  │  State Machine Engine  │  │
│  │  (rig.yaml) │  │   monitor   │  │  (TPT + SCXML fusion)  │  │
│  └──────┬──────┘  └──────┬──────┘  └────────────┬───────────┘  │
│         │                │                       │             │
│         ▼                ▼                       ▼             │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  Event bus + temporal-assertion monitors (reactive)      │  │
│  └──────────────────────────────────────────────────────────┘  │
└──────┬─────────────────────────────────────────────────────────┘
       │ adapters (shipped in v1)
       ▼
┌────────────────────────────────────────────────────────────────┐
│  Live system:  supervisor gRPC · Tracer.hh feed · CAN bus      │
└────────────────────────────────────────────────────────────────┘
```

The Robot library is **a routing layer, not a logic layer**. Every
keyword does:

```python
@keyword("Wait For State")
def wait_for_state(self, name, within="5s"):
    return self.runtime.expect(EventKind.STATE_ENTERED, name,
                               timeout=_seconds(within))
```

Three lines. All polling, timeouts, FSM stepping, parallel monitor
scheduling, trace tailing live in `runtime`.


## Rig description — YAML, not a new DSL

We need a way to describe the rig (resources, topology, services,
supervision, faults). v2 proposed a textX `.theia` grammar. v3 uses
**plain YAML** instead.

Reasons:

- YAML has tooling everywhere (LSP, schema, formatters)
- No grammar to build, maintain, evolve
- No artheia entanglement risk (YAML is just data)
- Pydantic / dataclass loaders give us types + validation for free
- HIL labs already use YAML rig descriptions (labgrid does, plus most
  CI systems' machine inventory) — operators read it without training

```yaml
# rigs/demo.yaml
version: 1
name: demo3way

resources:
  machines:
    compute_host: { transport: local }              # SIL default
    # compute_host: { transport: ssh, addr: 10.10.0.5 }   # HIL flip
  buses:
    tipc_cluster: { kind: tipc }

services:
  - name: sm
    binary: //services/sm:sm_daemon
    on: compute_host
    depends_on: []
    lifecycle: { startup_timeout: 5s, restart: permanent }
  - name: com
    binary: //services/com:com_daemon
    on: compute_host
    depends_on: [sm]
    lifecycle: { startup_timeout: 5s, restart: permanent }
  - name: gateway
    binary: //gateway:pero_cmp_gw
    on: compute_host
    depends_on: [sm, com]
    lifecycle: { startup_timeout: 15s, restart: permanent }

supervision:
  compute_domain:
    on: compute_host
    strategy: rest_for_one
    children: [sm, com, exec, perception]
    health_policy:
      restart_limit: { count: 3, within: 30s }
      escalation: node_restart

flows:
  RestartChild:
    params: [target]
    states:
      Running:
        entry: { action: supervisor.restart_child, args: [target] }
        transitions:
          - to: Restarted
            when: "service.state(${target}) == RUNNING and \
                   service.restart_count(${target}) >= prior + 1"
          - to: Failure
            when: "timeout(10s)"
    final: [Restarted, Failure]

assertions:
  gateway_recovery_timing:
    eventually: { expr: "service.state(gateway) == HEALTHY",
                  within: 5s }
    never:      { expr: "supervision.state(compute_domain) == DEADLOCK" }
    always:     { expr: "service(sm).heartbeat_period < 100ms",
                  while: "flow(RestartChild).active" }

faults:
  LostKCAN:
    inject:  { call: bus.disconnect, args: [kcan] }
    recover: { call: bus.reconnect, args: [kcan] }
    expect:
      - { expr: "service(gateway).state == DEGRADED", within: 2s }
      - { expr: "trace.event('bus_loss') on sm",      within: 1s }
```

Pydantic schema (`rf_theia/runtime/rig_schema.py`) gives instant
load-time validation; LSPs (YAML language server + a JSON schema) give
in-editor completion.

This file is **read** by the runtime — not by Robot. The keyword
`Load Rig rigs/demo.yaml` calls `runtime.load(path)`, which:

- parses + validates YAML
- builds the resource / service / supervision graphs
- compiles flows into FSM objects
- registers assertion monitors (idle until armed by `Assert ...`)
- connects adapters: supervisor gRPC per machine, trace tails per
  service, bus handles per declared bus

The Robot author never sees any of this.


## The keyword surface — DSL by naming

Robot constraints mean we can't have nested syntax. We can have
**structured names** and **context-sensitive arguments**.

Group keywords by *role*, not by *adapter*:

### Lifecycle (rig setup / teardown)

```
Load Rig              path
Tear Down Rig
Reserve Resource      name    mode=exclusive|shared
Release Resource      name
```

### Provisioning (declarative — runtime owns the FSM)

```
Provision             service_or_machine
Provision And Wait    service_or_machine    within=30s
Assert Healthy        target
Assert Unhealthy      target
```

### State machines (TPT/SCXML surfaced as keywords)

```
Start State Machine   flow_name    [key=value ...]
Stop  State Machine   flow_name
Emit Event            event_name   on=target   [payload...]
Wait For State        state_name   within=5s   in=flow_name
```

`Start State Machine` doesn't take a Python class — it names a flow
**from the rig YAML**. The runtime instantiates and runs it
asynchronously. `Wait For State` reactively blocks the Robot scenario
until the named state is entered.

This is the v2 SCXML idea preserved without needing a new grammar.

### Temporal assertions (TPT-flavored, declarative)

```
Assert                assertion_name
Assert Eventually     expression    within=5s   [in=flow_name]
Assert Always         expression    [while=...]
Assert Never          expression    [during=...]
Assert Within         expression    bounds=10ms..50ms
```

Note `Assert assertion_name` — fires a *named* assertion that's
already defined in the rig YAML. This keeps the Robot file readable
when assertions are intricate.

The expression sublanguage (`service.state(gateway) == HEALTHY`,
`service(sm).heartbeat_period < 100ms`) is evaluated by the runtime
against a live evaluation context. asteval handles it safely — same
pattern as TPT engine guards.

### Signal flow (TPT idioms preserved)

```
Set Signal           name    value
Ramp Signal          name    start    end    duration=2s
Hold Signal          name    value
Wait Until Signal Condition    expression    timeout=5s
```

These are the keywords your example showed. They map 1:1 onto the
existing TPT engine (vendored in v1).

### Distributed actors (TTCN-3 style)

```
Run Component        name    on=machine
Run Testcase         component.testcase
Send                 port    msg=...
Receive On           port    expected=...    within=2s
Verdict              pass|fail|inconclusive|error
```

Actors are still PYTHON CLASSES (rf-tpt-ls precedent), but instantiated
and bound per the rig YAML's `actors:` section. Keyword calls dispatch
through the runtime, not directly into actor methods.

### Faults

```
Inject Fault         name
Recover Fault        name
```

Both are calls into the rig YAML's `faults:` definitions. The runtime
runs the declared `inject` / `recover` actions and arms the `expect:`
monitors automatically.


## Why this satisfies the design principles you stated

| Principle | How v3 satisfies it |
|---|---|
| Declarative first | Rig YAML describes structure; keywords NAME flows/assertions/faults; runtime executes. |
| Reactive second | `Wait For State`, `Assert Eventually` are reactive — runtime watches event bus, fires the keyword's return. |
| Imperative last | The only imperative-looking keywords (`Set Signal`, `Emit Event`) are stimulus generators, not control flow. |
| First-class concepts | Machines, services, supervision, buses, flows, faults, assertions all live in YAML as named entities — never inlined into scenarios. |
| No keyword spaghetti | Sleep/poll/sleep/poll never appears in user code — runtime owns timing. |


## SIL vs HIL — same scenario, different rig YAML

A scenario is portable across SIL and HIL **provided** it only names
things from the rig YAML. The rig YAML changes:

```yaml
# rigs/demo.sil.yaml — local processes
resources:
  machines:
    compute_host: { transport: local }
```

```yaml
# rigs/demo.hil.yaml — real hardware
resources:
  machines:
    compute_host:
      transport: ssh
      addr: 10.10.0.5
      power: { driver: digi_power, port: 3 }
      serial: /dev/ttyUSB0
    hercules_ecu:
      transport: none      # bus-only, Cortex-R4F
      power: { driver: digi_power, port: 5 }
      bus: kcan
  buses:
    kcan: { kind: can, interface: vector.channel.1, bitrate: 500000 }
    tipc_cluster: { kind: tipc, machines: [compute_host, shwa_host] }
```

The scenario:

```robot
SM Crash Recovers
    Load Rig    %{RIG_FILE}                 # parameterized
    Start State Machine    RestartChild    target=sm_daemon
    ...
```

CI runs the SIL rig (no hardware). The lab runs the HIL rig (digi
power, real CAN). Same `.robot` file. The runtime + adapter set differ
under the hood.


## Where Robot stops being the right surface

You raised this directly. Here's the line:

**Robot stays the right surface as long as the scenario can be read as
a sequence of role-named keyword calls that NAME entities defined
elsewhere.** Concretely, while these properties hold:

1. **Scenario length stays bounded.** ~10–30 keywords per test case.
   Beyond that, the test is doing too much and should be split.
2. **No imperative branches.** No `IF`/`FOR` in user-written scenarios.
   If a test wants conditional behavior, it's two test cases with
   different tags, or it's a flow in the rig YAML.
3. **No raw temporal math.** `Wait For State within=5s` is fine.
   `Sleep 1s` followed by `Check X` is not.
4. **Assertions are named or expression-based.** No string-matching
   parser logic inside the test (Robot's regex/`Should Match` is for
   substring asserts, not protocol parsing).

When ANY of these break, the smell is: the test is becoming a
program. At that point we either move it into the runtime as a
reusable flow/component, or it becomes a Python `pytest` test that
imports `rf_theia.runtime` directly — bypassing Robot.

So Robot has a **graduation path**, not a hard ceiling. The runtime
API is the same either way. Robot is the productive default for
~90% of tests; pytest is the escape hatch for the messy 10%.

### The non-Robot surfaces we keep open

| Surface | When to use |
|---|---|
| `.robot` | Default. Most scenarios. CI gates. Acceptance tests. |
| `pytest` against `rf_theia.runtime` | Tests with non-trivial Python control flow, fixtures, parameterization. |
| MCP-driven Claude sessions | Bug investigation (create → run → inspect cycles). |
| HIL-only Python scripts | Lab bring-up, one-shot diagnostics. |

All four surfaces target the same runtime. The runtime is the product;
Robot is one of multiple surfaces.


## What changes in the v1 code (concretely)

### Stays unchanged

- `testing/` location, venv, requirements, MCP server.
- `tpt_engine/`, `space/`, `assessment/` (vendored).
- `adapters/supervisor_grpc.py`, `adapters/tracer_jsonl.py`.
- Existing `selftest/keywords_load.robot` (hermetic, still passes).

### Splits

`TheiaTestLibrary.py` splits into:
- `rf_theia/library.py` — thin keyword routing (no logic)
- `rf_theia/runtime/` — new package, all semantics
  - `runtime/loader.py` — YAML rig loader (Pydantic schemas)
  - `runtime/event_bus.py` — internal pub/sub
  - `runtime/flow_engine.py` — FSM executor (TPT-engine-derived)
  - `runtime/assertion_monitor.py` — eventually/always/never watchers
  - `runtime/expr.py` — asteval-based expression evaluator
  - `runtime/topology.py` — resource + service + supervision graph
  - `runtime/actor.py` — TTCN-3-style component base class

### Phase-1 keywords that go internal

| v1 keyword | v3 disposition |
|---|---|
| `T Sup Connect` / `Disconnect` | Internal — `Load Rig` opens, `Tear Down Rig` closes. |
| `T Sup Restart Child` | Internal — wrapped by `Emit Event crash` in a flow OR by a `Provision` call. |
| `T Sup Expect Child State` | Internal — used by `Assert Healthy` and `Wait For State`. |
| `T Sig Open Trace` / `Close Trace` | Internal — `Load Rig` does this per service. |
| `T Sig Expect Trace` | Renamed → `Wait For Event` (more general). |
| `T Sig Expect Order` / `Latency` | Internal — assertion-monitor primitives. |
| `Create Partition` / `Add Transition` / `Set Signal` / `Apply Ramp` | KEPT (they're already TPT-shape, declarative). |


## Phased rollout, revised

### Phase 2A — runtime split + rig YAML

1. Pydantic schema for rig YAML (`rf_theia/runtime/rig_schema.py`).
2. `Load Rig` / `Tear Down Rig` keywords; runtime loads + validates.
3. Split `TheiaTestLibrary` → `library.py` + `runtime/`; keep all v1
   tests green.
4. Port `restart_child.robot` to the v3 keyword shape.

### Phase 2B — flow engine + state-machine keywords

5. `runtime/flow_engine.py` reuses the TPT engine internally but
   accepts YAML-declared FSMs.
6. Keywords: `Start State Machine`, `Emit Event`, `Wait For State`,
   `Stop State Machine`.
7. Port `sm_broadcast.robot` to the v3 shape.

### Phase 2C — temporal assertion monitors

8. `runtime/assertion_monitor.py` — eventually/always/never watchers
   running as background threads, wired to the event bus.
9. Expression evaluator (`runtime/expr.py`, asteval).
10. Keywords: `Assert Eventually`, `Assert Always`, `Assert Never`,
    `Assert` (named).

### Phase 2D — fault keywords

11. Fault registry in runtime.
12. `Inject Fault`, `Recover Fault` keywords.

### Phase 3 — distributed actors (TTCN-3 style)

13. `runtime/actor.py` base class.
14. SSH transport for remote actors.
15. `Run Component`, `Run Testcase`, `Verdict` keywords.

### Phase 4 — HIL bring-up

16. labgrid integration for power + bus.
17. `transport: none` machine semantics.
18. Reservation primitives.


## Decisions captured

- **No new grammar.** Rig description is YAML + Pydantic schema.
- **No artheia coupling.** rf-theia reads artheia's *outputs* (rig.json,
  netgraph.json, manifest) but never imports artheia internals.
- **Robot keywords are the DSL.** Context-sensitive naming + the
  scenario structure carry the model.
- **Runtime owns all semantics.** Library is routing only.
- **Robot has a graduation path.** When tests outgrow Robot, the same
  runtime is reachable from pytest / MCP / standalone Python.
- **SIL vs HIL = rig YAML diff, not scenario diff.**


## Memo

The deeper lesson from v1 → v2 → v3: I kept trying to express "model"
in the language closest to me at the moment — first keywords, then
textX. Both were wrong. v1 stuffed the model into the test scenarios
(control-flow trap). v2 reinvented artheia's wheel inside the test
framework (SUT-entanglement trap). v3 separates the three things that
were getting conflated:

- **The model of the SUT** — that's artheia.
- **The model of the test rig** — that's YAML+Pydantic, owned by
  rf-theia, not artheia.
- **The scenario** — that's Robot keywords naming entities from the
  rig YAML.

Each layer has its own language, its own toolchain, its own evolution.
No cross-imports. No shared parsers. No shared identifiers (except
through stable wire formats: netgraph.json, rig.yaml, gRPC).

# rf-theia v2 — DSL design pivot

> **SUPERSEDED 2026-05-24 by `testing_framework_v3.md`.**
> v2's proposal to build a textX `.theia` grammar through artheia's
> pipeline was rejected: it entangles the test framework with the SUT
> toolchain. v3 keeps the five-layer conceptual model (resources /
> supervision / flows / actors / temporal) but moves the DSL surface
> back into Robot Framework with context-sensitive keyword families,
> and replaces the `.theia` grammar with a YAML rig file + Pydantic
> schema. v2 stays here for historical reference of the conceptual
> shape; do NOT implement against this doc.

Author: claude+roman · 2026-05-24
Supersedes (in part): `testing_framework.md` v1 (Phase 1 ships unchanged;
this doc shapes Phase 2+).

## Why v2

v1 shipped a keyword library. Phase 1 works — selftest 3/3 PASS, dryrun
6/6 PASS — but the surface is the wrong shape for what theia actually
needs to test. The corrective input crystallizes the gap:

> Provisioning/orchestration/supervision systems become difficult when
> **test flow = control flow** inside Robot files.

A keyword library makes every test author re-invent control flow: open
trace, restart child, sleep, poll, assert, close. The harness has to
**own** the orchestration; tests only describe **what** should be true.

What we want, distilled:

| Layer | Borrowed from | Theia analogue |
|---|---|---|
| topology + resources | labgrid | machines + buses + ECUs |
| service / supervision graph | OTP / your supervisor | gateway, sm, com, exec, perception + restart strategy |
| orchestration FSMs | SCXML | provisioning + recovery flows |
| distributed actors + verdicts | TTCN-3 | tester components on test rig machines |
| temporal assertions | TPT | `eventually`, `always`, `never` over trace + state |

These five layers are first-class. Everything else is library.


## What stays from v1

- `testing/` location, `rf_theia` package, own venv.
- TPT engine (vendored), space (parked), assessment (pandas helpers).
- `tools/supdbg/` reuse for supervisor gRPC.
- `Tracer.hh` text-format parser.
- MCP server shape + `.mcp.json` workspace integration.
- Robot Framework as the **scenario runner** — Robot stays, but in a
  reduced role.


## What changes

### The split: declarative DSL + Python engine + Robot scenarios

```
.theia (DSL)              describes topology + supervision + flows + assertions
   │
   ▼
rf_theia.engine           compiles + runs the orchestration model
   │
   ▼
rf_theia.actors           distributed test actors (TTCN-3-style)
   │
   ▼
.robot (scenarios)        declarative cases that REFERENCE the model
                          (one .theia model, many .robot test cases)
```

**Robot files become trivially short.** They name a model, invoke
flows, declare assertions, return verdicts. No control flow, no sleeps,
no polling loops, no adapter management.

### Concretely, what a Robot test looks like

Before (v1 — wrong shape, but it's what we shipped):

```robot
Restart Child Increments Counter
    T Sup Connect    localhost:5051
    T Sup Restart Child    sm_daemon
    T Sup Expect Child State    sm_daemon    RUNNING    within=10s
    T Sup Expect Restart Count   sm_daemon    1         within=10s
    T Sup Disconnect
```

After (v2 — declarative reference into the model):

```robot
Restart Child Increments Counter
    Use Model    rigs/demo.theia
    Run Flow     RestartChild    target=sm_daemon
    Assert       gateway_recovery_timing   # named assertion in .theia
    Verdict      pass
```

### The .theia DSL (the heart of v2)

Single declarative file describing the rig as a hierarchy of first-class
concepts. Phase 2 implements a textX grammar (artheia already proves the
toolchain).

```theia
// rigs/demo.theia — Demo3Way rig
//
// resources → topology → services/supervision → flows → assertions

resources {

  machine compute_host {
    addr     : "10.10.0.5"
    transport : ssh
  }

  machine shwa_host {
    addr     : "10.10.0.6"
    transport : ssh
  }

  bus tipc_cluster {
    kind : tipc
  }
}

topology {
  compute_host connects tipc_cluster
  shwa_host    connects tipc_cluster
}

service sm {
  binary  : "//services/sm:sm_daemon"
  on      : compute_host
  lifecycle {
    startup_timeout : 5s
    restart         : permanent
  }
}

service com {
  binary  : "//services/com:com_daemon"
  on      : compute_host
  depends_on : [sm]
  lifecycle {
    startup_timeout : 5s
    restart         : permanent
  }
}

supervision compute_domain on compute_host {
  strategy : rest_for_one
  children : [sm, com, exec, perception]
  health_policy {
    restart_limit : 3 within 30s
    escalation    : node_restart
  }
}

flow ProvisionRig {

  state Cold {
    transition -> Booting when supervisor.reachable(compute_host)
  }

  state Booting {
    transition -> Healthy
      when supervision.all_running(compute_domain)
    transition -> Failure when timeout(30s)
  }

  final Healthy
  final Failure
}

flow RestartChild(target : ServiceRef) {

  state Running {
    entry { supervisor.restart_child(target) }
    transition -> Restarted
      when service.state(target) == RUNNING
         and service.restart_count(target) >= prior_restart_count + 1
    transition -> Failure when timeout(10s)
  }

  final Restarted
  final Failure
}

assertion gateway_recovery_timing {

  eventually { service.state(gateway) == HEALTHY } within 5s

  never { supervision.state(compute_domain) == DEADLOCK }

  always {
    service(sm).heartbeat_period < 100ms
  } while flow(RestartChild).active
}

fault LostKCAN {

  inject  { bus.disconnect(kcan) }
  recover { bus.reconnect(kcan) }

  expect {
    service(gateway).state == DEGRADED within 2s
    trace.event("bus_loss") on sm within 1s
  }
}
```

The file is **declarative end-to-end**. There is no "open trace, call
restart, sleep, check" anywhere. The engine knows from the model:

- which trace files to tail (services × machines)
- when to open them (when an assertion or flow references them)
- when to connect supervisor gRPC (when a flow uses supervisor.*)
- which timeouts apply where (lifecycle.startup_timeout, etc)
- how to interpret `eventually` / `always` / `never` over time

### The engine

`rf_theia.engine` does the heavy lifting:

- **parses** `.theia` → AST (textX, identical to artheia's pipeline)
- **compiles** into runtime graph:
  - resource graph (machines, buses)
  - service graph (with dependencies)
  - supervision graph (strategies, restart policies)
  - flow state machines (compiled from `flow` blocks)
  - assertion monitors (compiled from `assertion` blocks)
- **runs** scenarios:
  - flow state machine drives orchestration
  - assertion monitors run in parallel; each is an independent watcher
  - actors execute on bound machines (TTCN-3-style; ssh + python agent
    or local thread)
  - verdicts collect into `pass / fail / inconclusive / error`

### Distributed actors

```theia
component GatewayTester runs on compute_host {

  port supervisor : grpc_supervisor("localhost:5051")
  port trace      : tracer_feed("/var/log/sm_daemon.log")
  port bus        : socketcan("can0")

  timer startup_timeout

  testcase SmCrashRecovery {

    startup_timeout := 30s
    startup_timeout.start

    bus.send({ id: 0x120, payload: [01 02 03] })

    alt {
      [] supervisor.event("sm_crash") {
           verdict pass
         }
      [] startup_timeout.timeout {
           verdict fail
         }
    }
  }
}
```

The actor has typed ports onto the live system, an event-driven `alt`
matcher (TTCN-3 idiom), and verdicts. The runtime takes care of
deploying the actor onto the named machine and wiring its ports to the
real adapters.

### Where the keyword library goes

The v1 `TheiaTestLibrary` keywords **don't disappear** — they become the
**bottom-tier primitives** the engine uses internally. Tests no longer
call them. So:

- `T Sup *`, `T Sig *` → internal `rf_theia.engine.adapters.*` modules
  (already named that — minor renames at most).
- `T Wait` → internal only; never used in user-facing scenarios.
- A small set of **declarative** Robot keywords replaces them:

```
Use Model         path/to/rig.theia
Run Flow          flow_name    [arg=value ...]
Provision         service_or_machine
Inject Fault      fault_name
Assert            assertion_name
Run Component     component_name
Run Testcase      component.testcase
Verdict           pass|fail|inconclusive
```

That's it. ~8 keywords, all declarative, no control flow.


## Use-case shapes — SIL and HIL

The DSL has to make both modes the same `.theia` file with different
resource sections. This is where the labgrid+TTCN-3+OTP fusion pays off.

### SIL — all on a single Linux dev box

```theia
resources {
  machine localhost {
    transport : local      // engine spawns processes directly
  }
  bus tipc_cluster { kind : tipc }
}

service sm     { binary : "//services/sm:sm_daemon"  on : localhost ... }
service com    { binary : "//services/com:com_daemon" on : localhost ... }
service exec   { binary : "//services/exec:exec_daemon" on : localhost ... }

// Same supervision + flows + assertions as HIL.
```

The engine notices `transport : local`:
- spawns binaries directly with the supervisor running locally
- reads Tracer.hh stderr in-process (no ssh, no file tail)
- gRPC supervisor connects to `localhost:5051`

Use case: PR gates, fast iteration on supervisor logic, gen_statem
behavior tests, signal-flow assertions across FCs. Runs in CI without a
lab.

### HIL — multi-ECU rig with real hardware

```theia
resources {

  machine compute_host {
    addr      : "10.10.0.5"
    transport : ssh
    power     : digi_power.port(3)      // labgrid-style
    serial    : uart("/dev/ttyUSB0")
  }

  machine shwa_host {
    addr      : "10.10.0.6"
    transport : ssh
    power     : digi_power.port(4)
  }

  machine hercules_ecu {        // Cortex-R4F, can't run Python — bus-only
    transport : none
    power     : digi_power.port(5)
    bus       : kcan
  }

  bus kcan {
    kind      : can
    interface : vector.channel(1)
    bitrate   : 500000
  }

  bus tipc_cluster {
    kind      : tipc
    machines  : [compute_host, shwa_host]
  }
}

// Power-on / reset is now a first-class precondition for flows.
flow ProvisionRig {
  state Cold {
    entry { resources.power_on_all }
    transition -> Booting when machine.reachable(compute_host)
  }
  ...
}

fault LostKCAN {
  inject  { bus.disconnect(kcan) }      // labgrid-side bus shutdown
  recover { bus.reconnect(kcan) }
  expect {
    service(gateway).state == DEGRADED within 2s
    trace.event("bus_loss") on sm within 1s
  }
}
```

Use case: lab acceptance test of a new gateway build before deployment,
fault-injection on real CAN bus, multi-ECU cluster validation.

### The point

**One DSL file describes both.** Changing the `resources {}` block is
the only diff between SIL and HIL. Flows, supervision, services,
assertions, components — all reusable.

The compiler emits the same runtime graph; the adapters bound to it
differ:

- `transport : local` → `LocalProcessAdapter`, `StderrTraceAdapter`
- `transport : ssh`   → `SSHProcessAdapter`, `RemoteTraceAdapter`,
                        `RemoteSupervisorGrpcAdapter`
- `transport : none`  → bus-only; no in-band observation; only
                        labgrid-side power/bus control + indirect
                        observation via neighboring machines


## Cleanup from the original v1 — what we drop

Per your guidance, prune anything that doesn't fit theia's actual
testing domain:

- **No bootloader flashing**. theia binaries are deb/ipk-packaged; the
  install path is provisioning, not OS-level firmware updates.
- **No CAN frame fault injection in the DSL**. CAN faults live as
  `fault {}` blocks invoking `bus.disconnect` / `bus.reconnect` (labgrid
  primitives), not as hand-rolled frame manipulation. If we ever need
  fuzz/replay, that's a `bus.replay(file)` primitive — not 50 lines of
  Python in a Robot test.
- **No low-level UART scripting**. UART is observable via
  `machine.serial.contains("...")` predicate in flows; no `read_line`,
  no `expect_prompt` keyword spam.


## Syntax philosophy — what every construct must satisfy

> declarative first, reactive second, imperative last

Concretely:

- **GOOD**: `eventually { service.state(gateway) == HEALTHY } within 5s`
- **BAD**: `wait 5s; check gateway state`

- **GOOD**: `flow.RestartChild(sm_daemon)` — the engine knows the FSM
- **BAD**: `restart sm_daemon; sleep 2; check; sleep 2; check; …`

- **GOOD**: `fault LostKCAN { inject {...} expect {...} recover {...} }`
- **BAD**: `disconnect can; wait; assert state; reconnect can`

- **GOOD**: `verdict pass` — TTCN-3 verdict, surfaces to Robot
- **BAD**: `Should Be Equal As Strings ${state} HEALTHY`

If a construct can't be written declaratively, it goes in the engine,
not in the DSL surface or the Robot scenario.


## Phased rollout from v1 → v2

Phase 1 (DONE) ships as the **foundation**. Don't rip it out — the
keyword library becomes the engine's bottom layer. Renames only.

### Phase 2 — the `.theia` DSL + engine MVP

1. Grammar: `testing/rf_theia/grammar/theia.tx` — textX-based, models:
   - resources / machine / bus
   - service / supervision (mirrors theia's existing supervisor)
   - flow + state + transition (SCXML-shaped)
   - assertion + eventually/always/never
   - fault + inject/expect/recover
2. AST + loader: `rf_theia/model/`, mirrors `artheia/model/`.
3. Engine MVP: `rf_theia/engine/`:
   - resource graph builder
   - flow runtime (FSM executor)
   - assertion monitors (parallel watchers)
   - actor dispatcher (local-only for MVP — `transport : local`)
4. The 8 declarative Robot keywords: `Use Model`, `Run Flow`,
   `Provision`, `Assert`, `Verdict`, etc.
5. Port the existing 3 smoke scenarios to the new shape. Net code in
   the Robot scenarios should drop by ~70%.

### Phase 3 — distributed actors + remote transports

6. Component grammar: `component … runs on … { port … testcase … }`.
7. SSH transport + remote actor agent (small Python process pushed
   over ssh, talks back over a control socket).
8. Bus adapters (socketcan first; vector via a labgrid bridge).

### Phase 4 — fault injection + temporal logic completeness

9. `fault {}` engine: lifecycle inject → observe expect → recover.
10. Temporal operators: `until`, `since`, `while`, plus the existing
    `eventually/always/never`.
11. Verdict aggregation across components.

### Phase 5 — HIL bring-up

12. labgrid integration for real hardware power + bus control.
13. `transport : none` machines (Hercules ECU): bus-only observation.
14. Reservation primitives (`reserve { ECU_A exclusive }`) for shared
    labs.


## Implementation architecture (engine internals)

```
.theia source
   │  textX parse + inheritance flatten (same toolchain as artheia)
   ▼
TheiaModel (Python AST)
   │  compile
   ▼
RuntimeGraph
   ├─ ResourceGraph     (machines, buses, power)
   ├─ ServiceGraph      (binaries, dependencies, lifecycle)
   ├─ SupervisionGraph  (strategies, restart policies)
   ├─ FlowMachines      (one FSM per `flow` block)
   ├─ AssertionMonitors (one watcher per `assertion` block)
   └─ ComponentBindings (actor → machine pinning)
   │  execute
   ▼
Runtime
   ├─ adapters/         (the renamed v1 supervisor_grpc, tracer_jsonl,
   │                     local_process, ssh_process, socketcan, ...)
   ├─ event_bus/        (in-process pub/sub for flows, monitors, actors)
   ├─ verdict_collector (TTCN-3-style verdict semantics)
   └─ trace_recorder    (writes a structured run trace for assessment/)
```

The Robot scenario is a thin client over this runtime. Per scenario it:

1. loads a model (`Use Model`)
2. asks the runtime to start a flow or component (`Run Flow`, `Run Testcase`)
3. registers assertions (`Assert`)
4. collects the verdict

That's the entire user-facing surface.


## Open design questions

These are NOT blocking decisions for v1 → v2. They get answered before
phase 2's grammar lands.

1. **Where does the .theia file live for the demo3way rig?**
   Likely `rigs/demo.theia` at workspace root, next to (or under)
   `platform/system/system.art`. Keeps rig description close to the
   system description it tests.

2. **Should `service {}` be inferred from artheia's existing
   `service.py` definitions?** Probably yes — the engine can read
   `services/manifest/service.py` and synthesize the service section
   automatically. Test author only writes what differs (faults,
   assertions). This avoids duplication.

3. **How tightly do flows couple to the artheia statem grammar?**
   Aim: the artheia `statem { ... }` block (used by gen_statem-aware
   nodes) and rf-theia's `flow { ... }` share enough syntax that
   someone reading both files doesn't context-switch. Different
   *semantics* (one models the SUT, one models the test orchestrator),
   same idioms.

4. **Verdict semantics under faults**: TTCN-3 has a verdict-merge
   ordering (`error > fail > inconclusive > pass > none`). Adopt that
   directly — no need to invent.


## Memo to future me

The reason v1 felt "off" the moment it shipped: **a Robot keyword
library makes every test author re-implement what should be the
harness's job**. Open the trace, restart the child, sleep, poll, close.
Every test author writes the same five lines. After 50 tests that's
250 lines of harness code in user space.

v2 reverses the polarity: the harness reads the .theia model and knows
what to open, when, and how. The test says "given this model, when
RestartChild flow runs, assert gateway_recovery_timing". That's it.

The keyword library wasn't wrong as a starting point — but treating it
as the **product** would have boxed us into a control-flow-in-tests
trap, which is exactly the failure mode you flagged.

# rf-theia — Robot Framework testing harness for the theia/artheia stack

Author: claude+roman · 2026-05-24

A Robot Framework testing framework specific to theia. Models the rf-tpt-ls
ancestor (in `up/rf_tpt_ls/`) but retargets the keyword surface at theia's
five testable subsystems: **provisioning**, **orchestration**, **supervision**,
**signal-flow**, **artheia generators**.

TPT (Time Partition Testing) stays in the engine — it's how we drive
time-shaped scenarios into the signal-flow tests. SPT (Space Partition
Testing) is carried forward from rf-tpt-ls but not exercised yet (no
spatial domain in theia today; the engine is there for later).

A dedicated TPT *textual* DSL is **not** in scope — see
`docs/tasks/BACKLOG/TPT-DSL.md`. Time partitions are expressed in Python
inside the keyword library, surfaced via the existing TPT keywords
(`Create Partition`, `Add Transition`, `Apply Ramp`, `Run Time Engine`).


## Goals

- **One conversation, full loop**: an MCP-driven create → run → inspect →
  fix cycle in Claude Code, just like rf-tpt-ls.
- **Reuse**: don't re-implement TPT, BT-adapter, or MCP plumbing — vendor
  the proven pieces from `up/rf_tpt_ls/`.
- **Five DSLs, one library**: each subsystem gets a thin keyword family
  with a consistent prefix (`T Sup ...`, `T Sig ...`, `T Art ...`,
  `T Prov ...`, `T Orch ...`). Same library class, dispatched by prefix.
- **Live + offline**: signal-flow tests should run against a live
  supervisor + trace stream; generator-regression tests should run
  hermetically with no processes.


## Name and layout

```
testing/                                  # NEW top-level dir
├── rf_theia/                             # Python package
│   ├── __init__.py
│   ├── TheiaTestLibrary.py               # main keyword library
│   ├── adapters/
│   │   ├── supervisor_grpc.py            # reuse tools/supdbg/_gen
│   │   ├── tracer_jsonl.py               # Tracer.hh JSONL consumer
│   │   ├── artheia_cli.py                # subprocess wrapper around `artheia ...`
│   │   ├── puppet_dry_run.py             # provisioning hook
│   │   └── mcp_server.py                 # FastMCP, vendored shape from rf-tpt-ls
│   ├── tpt_engine/                       # VENDORED from up/rf_tpt_ls/tpt_engine/
│   ├── space/                            # VENDORED — SPT, parked, no theia usage yet
│   ├── assessment/                       # VENDORED — JSONL → pandas analysis
│   └── scenarios/
│       ├── supervision/
│       ├── signal_flow/
│       ├── artheia_gen/
│       ├── provisioning/
│       ├── orchestration/
│       └── selftest/
├── KEYWORDS.md                           # cheat sheet (rf-tpt-ls style)
├── TESTING.md                            # setup + run guide
├── requirements.txt
├── pyproject.toml
├── run_mcp.sh                            # MCP launcher
└── .mcp.json                             # workspace-root MCP config -> here
```

`testing/` (not `test/`) avoids visual collision with the per-component
`test/` dirs (`platform/runtime/test/`, `gateway/.../test/`, `artheia/tests/`).
Those stay component-local unit tests; `testing/` is the cross-component
e2e/integration harness.

Package import root is `rf_theia`. Robot scenarios import
`rf_theia.TheiaTestLibrary`.


## Reuse map: what comes from `up/rf_tpt_ls/`

| rf-tpt-ls module       | Goes where in rf-theia             | Why                                      |
|------------------------|------------------------------------|------------------------------------------|
| `tpt_engine/`          | `rf_theia/tpt_engine/` (vendor)    | Partitions/transitions/guards/signals — domain-agnostic, ship as-is. |
| `space/`               | `rf_theia/space/` (vendor)         | Parked. SPT primitives kept for future spatial testing — no current consumer. |
| `assessment/`          | `rf_theia/assessment/`             | Trace-JSONL → pandas analysis — directly usable on Tracer.hh's output. |
| `bt_adapter/`          | **dropped**                        | LS-specific (BehaviorTree.CPP). theia has no BT layer. |
| `adapters/mcp_server.py` | `rf_theia/adapters/mcp_server.py` | Same FastMCP shape, retarget tool list. |
| `adapters/ros2_*.py`   | **dropped**                        | LS-specific. |
| `LaserShieldLibrary.py`| **rewritten** as `TheiaTestLibrary.py` | Same `@keyword`-decorator structure, different domain methods. |
| `TESTING.md` / `KEYWORDS.md` | refreshed for rf-theia       | Doc shape carries over verbatim. |
| `run_mcp.sh`           | `run_mcp.sh`                       | Launcher pattern — workspace-relative paths. |


## Keyword surfaces (five DSLs, one library)

All keywords live on `TheiaTestLibrary`. Prefixes group them in the
Robot listing without forcing five separate library imports.

### `T Sup ...` — Supervision (Phase 1, first slice)

Reuses the gRPC stubs already generated under `tools/supdbg/_gen/`. The
`supdbg` Python CLI proves the wire works against a live supervisor.

| Keyword                         | Args                                  | Notes |
|---------------------------------|---------------------------------------|-------|
| `T Sup Connect`                 | `endpoint=localhost:5051`             | gRPC channel to services/com. |
| `T Sup Start Child`             | `name`                                | `StartChild` RPC. |
| `T Sup Restart Child`           | `name`                                | `RestartChild` RPC. |
| `T Sup Terminate Child`         | `name`                                | `TerminateChild` RPC. |
| `T Sup Expect Child State`      | `name`  `state`  `within=5s`          | Poll `ChildState` until match or timeout. |
| `T Sup Expect Restart Count`    | `name`  `count`  `within=10s`         | Watchdog-driven restart counter. |
| `T Sup Heartbeat Stale Within`  | `name`  `seconds`                     | Assert heartbeat staleness threshold reached. |
| `T Sup Get Topology`            |                                       | Returns supervisor tree as dict (snapshot for asserts). |

### `T Sig ...` — Signal-flow (Phase 1, first slice)

Drives messages into the running cluster and observes the Tracer.hh
JSONL stream to assert delivery, ordering, latency. TPT engine drives
time-shaped stimuli.

| Keyword                         | Args                                  | Notes |
|---------------------------------|---------------------------------------|-------|
| `T Sig Open Trace`              | `source=tcp://localhost:6000`         | Subscribe to the trace feed. |
| `T Sig Close Trace`             |                                       | Stop subscription, flush to buffer. |
| `T Sig Cast`                    | `node`  `msg_kind`  `**fields`        | Inject a cast over the bus into the named node. |
| `T Sig Expect Trace`            | `event`  `node=`  `within=2s`         | Assert a Tracer record matches within window. |
| `T Sig Expect Order`            | `events`  `same_correlation=True`     | Assert ordered sequence of trace events. |
| `T Sig Expect Latency`          | `from_event`  `to_event`  `lt=50ms`   | TPT-style temporal assertion across correlated traces. |
| `T Sig Filter Records`          | `where=...`                           | Returns matching records as pandas DataFrame (uses `assessment/`). |
| `T Sig TPT Run`                 | `partition`  `timeout=60s`            | Drives the TPT engine — stimuli + guard transitions. |

`T Sig TPT Run` builds on the existing TPT keywords (`Create Partition`,
`Add Transition`, `Set Signal`, `Apply Ramp`). Time partitions stay
Python-side; no new textual TPT grammar.

### `T Art ...` — Artheia generator regression (Phase 2)

Hermetic, no live processes. Drives `artheia gen-*` subcommands, diffs
output trees against golden fixtures, validates against the schema.

| Keyword                         | Args                                  | Notes |
|---------------------------------|---------------------------------------|-------|
| `T Art Parse`                   | `file`                                | Parse with loader. Returns flattened model dict. |
| `T Art Gen Proto`               | `file`  `out_dir`                     | Run `artheia gen-proto`, return file list. |
| `T Art Gen App`                 | `file`  `--kind`  `out_dir`           | Run `artheia gen-app --kind {fc,composition,...}`. |
| `T Art Gen Netgraph`            | `file`  `out`                         | Run `gen-netgraph` and `gen-psp-netgraph` independently. |
| `T Art Diff Against Golden`     | `out_dir`  `golden_dir`               | Tree-level diff with deny/allow patterns (e.g. ignore timestamps). |
| `T Art Expect Symbol In Header` | `header_path`  `symbol`               | Static-assertion-style check on emitted C++. |
| `T Art Expect Validator Error`  | `file`  `pattern`                     | Negative test: parsing should raise a validator error. |

Golden fixtures live under `testing/rf_theia/scenarios/artheia_gen/golden/`.
Update flow: `T Art Diff Against Golden` flags drift → human reviews
diff → `artheia` CLI flag `--update-golden` regenerates (out of scope
for the first cut; manual `cp -r` is fine initially).

### `T Prov ...` — Provisioning (Phase 3)

Tests the Puppet + manifest pipeline. Lower-frequency than signal-flow
tests; targets release/deployment gating.

| Keyword                         | Args                                  | Notes |
|---------------------------------|---------------------------------------|-------|
| `T Prov Audit Manifest`         | `rig=demo`                            | Runs `artheia audit-manifest`, expects clean exit. |
| `T Prov Emit Per Machine`       | `rig=demo`  `out_dir`                 | Runs the per-machine manifest emit step. |
| `T Prov Puppet Apply Dry Run`   | `manifest_dir`                        | `puppet apply --noop` against the emitted tree. |
| `T Prov Expect Package`         | `machine`  `package`                  | Assert `Machine.os_packages` resolves to a real .deb path. |
| `T Prov Expect Opkg`            | `machine`  `artifact`                 | Assert opkg artifacts list contains a target. |

### `T Orch ...` — Orchestration (Phase 3)

Two-phase orchestration: stage 1 (provision via Puppet), stage 2
(supervisor start). Tests the handover.

| Keyword                         | Args                                  | Notes |
|---------------------------------|---------------------------------------|-------|
| `T Orch Start Stack`            | `rig=demo`                            | Drives the orchestration entry point. |
| `T Orch Stop Stack`             |                                       | Cleanup. |
| `T Orch Expect All Machines Up` | `within=30s`                          | Poll until every machine's supervisor reports ready. |
| `T Orch Expect Cluster Ready`   | `within=60s`                          | All FCs heartbeating, cluster netgraph fully resolved. |
| `T Orch Slice By Machine`       | `machine`                             | Returns the per-machine execution.yaml as dict. |


## Adapters — how keywords reach into theia

| Adapter                           | Talks to                       | Reuse |
|-----------------------------------|--------------------------------|-------|
| `adapters/supervisor_grpc.py`     | services/com gRPC bridge       | `tools/supdbg/_gen` stubs + `tools/supdbg/client.py` patterns. |
| `adapters/tracer_jsonl.py`        | Tracer.hh JSONL stream         | New. Reads from TCP socket or file tail; deserializes per `platform/runtime/include/Tracer.hh`. |
| `adapters/artheia_cli.py`         | `artheia` console script       | Subprocess wrapper; respects venv-based invocation (see `project-build-invocation`). |
| `adapters/puppet_dry_run.py`      | Local `puppet apply --noop`    | Thin wrapper. |
| `adapters/mcp_server.py`          | Claude Code via MCP            | FastMCP shape from rf-tpt-ls. |


## MCP server — Claude Code integration (Phase 1, alongside first slice)

Mirror the rf-tpt-ls MCP setup. Workspace-root `.mcp.json` points at
`testing/run_mcp.sh` (which sources `.venv/`). FastMCP exposes:

| Tool                    | Purpose |
|-------------------------|---------|
| `run_scenario`          | Run a `.robot` file, return parsed pass/fail report. |
| `list_scenarios`        | Discover scenarios under `testing/rf_theia/scenarios/`. |
| `list_keywords`         | Return the keyword catalog (read from `KEYWORDS.md` + library introspection). |
| `create_scenario`       | Write a new `.robot` file from a Claude-supplied skeleton. |
| `get_test_results`      | Read the last `output.xml`, return summary + failed assertions. |
| `analyze_trace`         | Run `assessment/` over a captured trace JSONL, return metrics dataframe. |
| `tail_supervisor_log`   | Last N lines of supervisor stderr (debug helper). |

The bug-investigation flow from rf-tpt-ls's TESTING.md ports directly:
"describe bug → Claude creates reproducer → runs it → iterate".


## Phased rollout

### Phase 1 — Supervision + Signal-flow + MCP (Weeks 1–2)

1. Create `testing/` skeleton: `pyproject.toml`, `requirements.txt`,
   `rf_theia/__init__.py`, empty `TheiaTestLibrary.py`.
2. Vendor `tpt_engine/`, `space/`, `assessment/` from `up/rf_tpt_ls/`
   verbatim (no edits — they're domain-agnostic).
3. Implement `adapters/supervisor_grpc.py` reusing `tools/supdbg/_gen/`.
4. Implement `adapters/tracer_jsonl.py`.
5. Wire `T Sup` + `T Sig` keyword families to those adapters.
6. Port `adapters/mcp_server.py` from rf-tpt-ls; retarget tool list.
7. First scenarios (smoke):
   - `scenarios/supervision/restart_child.robot` — kill + watchdog assert.
   - `scenarios/signal_flow/sm_broadcast.robot` — drive SM, assert
     downstream cast traces.
   - `scenarios/selftest/keywords_load.robot` — every keyword resolves.
8. `.mcp.json` at workspace root → `code --add-mcp testing/run_mcp.sh`.
9. Update `MEMORY.md` with the `rf-theia` location.

### Phase 2 — Artheia generator regression (Week 3)

10. `adapters/artheia_cli.py` — subprocess wrapper with venv discovery.
11. `T Art ...` keyword family.
12. Capture initial golden trees for each FC + the supervisor + the
    demo3way composition.
13. Scenario: `scenarios/artheia_gen/all_fcs_regen.robot`.
14. CI hook (out of scope for this task — captured in follow-up).

### Phase 3 — Provisioning + Orchestration (Week 4+)

15. `adapters/puppet_dry_run.py`.
16. `T Prov ...` + `T Orch ...` keyword families.
17. Two-phase orchestration scenario.


## What this is NOT

- **Not a TPT textual DSL**. Time partitions live in Python via the
  TPT engine. The textual TPT design is parked in `BACKLOG/TPT-DSL.md`.
- **Not a replacement for unit tests**. `platform/runtime/test/`,
  `artheia/tests/`, etc. continue to own component-local correctness.
  rf-theia owns cross-component e2e.
- **Not a deployment tool**. Provisioning keywords test the pipeline;
  they don't replace `repo sync` + `bazel build`.
- **Not auto-included in `bazel test`**. rf-theia runs via Robot CLI
  (or MCP). A Bazel `py_test` wrapper is possible later but not the
  primary entry point.


## Open follow-ups (out of plan scope, listed for awareness)

- Bazel `py_test` rule wrapping `robot ...` for CI.
- Golden-fixture update flow (`artheia gen-* --update-golden` flag).
- SPT activation if theia ever grows a spatial domain (e.g. ADAS work).
- Trace JSONL TCP feed — currently file-tail only; supervisor would
  need a small TCP publisher for live streaming (Phase 1 can start
  with file-tail, upgrade later).


## References

- `up/rf_tpt_ls/TESTING.md` + `KEYWORDS.md` — the ancestor framework.
- `docs/tasks/BACKLOG/robot_fwk_tpt_testing.md` — TPT concept notes.
- `docs/tasks/BACKLOG/TPT-DSL.md` — textual TPT grammar parked.
- `tools/supdbg/` — proves the gRPC client + REPL pattern works.
- `platform/runtime/include/Tracer.hh` — the wire shape for signal-flow tests.

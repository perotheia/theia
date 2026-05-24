# rf-theia — testing harness for theia/artheia

Robot Framework + TPT engine retargeted at the theia stack. Five DSL
surfaces share one library: `T Sup ...`, `T Sig ...`, `T Art ...`,
`T Prov ...`, `T Orch ...`.

Plan and design rationale: `docs/tasks/BACKLOG/testing_framework.md`.
Keyword cheat-sheet: `KEYWORDS.md`.


## Setup

```bash
cd testing
python3 -m venv .venv
./.venv/bin/pip install -r requirements.txt
```

The venv lives at `testing/.venv/`. Activate per-shell or invoke the
binaries directly (`./.venv/bin/robot`, `./.venv/bin/python`). No global
install needed — keeps rf-theia hermetic from the artheia venv.


## Running scenarios

```bash
cd testing

# Hermetic selftest — no live theia needed.
PYTHONPATH=. ./.venv/bin/robot \
  --outputdir /tmp/rf_theia_output \
  rf_theia/scenarios/selftest/

# Live e2e — supervisor must be running + trace file must exist.
PYTHONPATH=. ./.venv/bin/robot \
  --outputdir /tmp/rf_theia_output \
  rf_theia/scenarios/supervision/

# Dryrun (parse + resolve keywords, don't execute) — catches typos in
# any scenario whether or not the live stack is up.
PYTHONPATH=. ./.venv/bin/robot --dryrun \
  --outputdir /tmp/rf_theia_dry \
  rf_theia/scenarios/
```

Filter by tag:

```bash
./.venv/bin/robot --include hermetic rf_theia/scenarios/
./.venv/bin/robot --exclude live    rf_theia/scenarios/
```

Tag conventions:

- `hermetic`  — no live processes required (selftest, generator regression).
- `live`      — needs supervisor + cluster + trace file.
- `supervision`, `signal-flow`, `artheia-gen`, `provisioning`, `orchestration`
  — one per DSL surface.


## MCP integration

`.mcp.json` at the workspace root points Claude Code at
`testing/run_mcp.sh`. The launcher activates the rf-theia venv before
exec'ing `rf_theia.adapters.mcp_server`.

Tools exposed: `run_scenario`, `list_scenarios`, `list_keywords`,
`create_scenario`, `get_test_results`, `analyze_trace`,
`tail_supervisor_log`.

Verify after install:

```bash
claude mcp list           # should show rf-theia ✓ Connected
```


## Bug investigation workflow

1. Describe the bug to Claude Code in plain English.
2. Claude calls `create_scenario` with a targeted `.robot` using the
   keyword families below.
3. Claude calls `run_scenario`, reads pass/fail.
4. Iterate: refine timing, add assertions, narrow the root cause.
5. If a passing reproducer is interesting, leave it under
   `scenarios/<surface>/` as a regression.


## Layout

```
testing/
├── pyproject.toml
├── requirements.txt
├── run_mcp.sh                  # MCP launcher (workspace-relative)
├── TESTING.md                  # this file
├── KEYWORDS.md                 # keyword cheat sheet
└── rf_theia/
    ├── TheiaTestLibrary.py     # the one library
    ├── adapters/
    │   ├── supervisor_grpc.py  # T Sup → tools/supdbg/_gen reuse
    │   ├── tracer_jsonl.py     # T Sig → Tracer.hh text-format parser
    │   └── mcp_server.py       # FastMCP shell
    ├── tpt_engine/             # vendored from up/rf_tpt_ls
    ├── space/                  # vendored, parked (SPT primitives)
    ├── assessment/             # vendored, pandas-style analysis
    └── scenarios/
        ├── selftest/           # hermetic
        ├── supervision/        # T Sup
        ├── signal_flow/        # T Sig
        ├── artheia_gen/        # T Art (phase 2)
        ├── provisioning/       # T Prov (phase 3)
        └── orchestration/      # T Orch (phase 3)
```


## What this is NOT

- Not a replacement for component-local unit tests. `platform/runtime/test/`,
  `artheia/tests/`, etc. still own per-component correctness.
- Not auto-included in `bazel test`. rf-theia runs via the Robot CLI
  (or MCP). A Bazel `py_test` wrapper may come later.
- Not a textual TPT DSL. Time partitions live in Python via the engine;
  textual TPT design parked at `docs/tasks/BACKLOG/TPT-DSL.md`.

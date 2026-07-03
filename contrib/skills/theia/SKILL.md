---
name: theia
description: Refresher for agents working in the theia framework — the AUTOSAR-Adaptive-style platform (Functional Clusters on the Theia C++ actor runtime, modeled in the artheia .art DSL). Orients you to the layout, the build, the consuming-workspace flow (theia init), and where to dig deeper. Args: (none) — read top-to-bottom, then load a reference under references/ for the task at hand.
---

Theia is a AUTOSAR-Adaptive-Platform-style
**Functional Clusters (FCs)** running as supervised processes on a custom
C++ **actor runtime** ("the Theia runtime"), with the whole system modeled
in the **artheia** `.art` DSL and built with Bazel.

It's a **standalone git repo** (`theia`) with `artheia/` and
`third_party/etcd-cpp-apiv3/` as **git submodules** — NOT a Google-`repo`
monorepo any more (that split happened; some on-disk `gateway/` / `vendor/`
dirs are gitignored legacy repo-sync leftovers, not part of the framework).
A user builds their own apps in a SEPARATE **consuming workspace** scaffolded
by `theia init` (see "Consuming workspaces" below). The in-repo demo lives in
exactly such a workspace at `demo/` — Theia dogfoods the `theia init` flow,
exercising the framework in CI.

Read this page to orient. For a specific task, load the matching reference:

| You're doing… | Read |
| --- | --- |
| **Removing / simplifying / rewriting code** (read first) | [references/requirements.md](references/requirements.md) — functional vs non-functional |
| Writing or editing `.art` files | [references/art-lang-grammar.md](references/art-lang-grammar.md) |
| Writing or editing a deploy rig (`manifest/<rig>/rig.py`): the DeploymentLayer / Append-Remove monoid algebra | [references/manifest-py-syntax.md](references/manifest-py-syntax.md) |
| Generating / regenerating an application C++ (lib/main/impl) | [references/artheia-gen-app.md](references/artheia-gen-app.md) |
| Regenerating the system: .art → manifests (stages 1–2) | [references/artheia-gen-system.md](references/artheia-gen-system.md) |
| Bazel build: rig extension, FC daemons, bundles | [references/build-system.md](references/build-system.md) |
| Provisioning & orchestration: the `theia manifest → dist → orchestrate` handoff flow (Ansible, per-machine `.deb`) | [references/provision-orchestrate.md](references/provision-orchestrate.md) |
| Deployment: the installable `.deb` framework (dev/runtime/services/workspace split, `theia release`, `/opt/theia`) | [references/deployment.md](references/deployment.md) |
| AUTOSAR PSP import: DBC/FIBEX → `.art` + netgraph | [references/autosar.md](references/autosar.md) |
| Config schema migration: per-node `config <Msg>` evolution, transform.json, gen-transform plugin, the dev workflow + RF wrapper | [references/migration.md](references/migration.md) |

Always run artheia with the workspace venv on PATH:
`PATH="$PWD/.venv/bin:$PATH" artheia …`. It is editable-installed
(`pip install -e artheia/`), so edits to `artheia/artheia/**.py` take
effect immediately.

## The abstraction ladder

Everything in `.art` rests on three primitives. Internalize them:

| Primitive | Is a | Owns |
| --- | --- | --- |
| **node** | thread | one TIPC `type/instance`, ports, a `GenServer`/`GenStateM`/`GenRunnable` |
| **composition** | process (one executable) | prototypes (node instances) + in-process wiring (`connect`) |
| **cluster** | distribution bundle | compositions + inter-process wiring; the deploy/packaging unit |


## Repository map

Standalone `theia` git repo (+ `artheia`/`etcd` submodules). Major dirs:

- **`system/`** — the **virtual root** of the `system` package and the
  aggregation point everything resolves against. `system/system.art`
  (`cluster Platform`); per-cluster symlinks: `system/services` →
  `../services/system/services` (one consolidated dir, holds
  `cluster.art` + per-FC symlinks), `system/supervisor`,
  `system/platform/runtime`. The framework root has **no app cluster** —
  `system.apps` (the demo) moved OUT to the `demo/` consuming workspace.
- **`services/`** — the FCs. 6 with real daemons live at
  `services/<fc>/system/<fc>/{package,component}.art` (spec) +
  `services/<fc>/{lib,main,impl}/` (generated + hand-owned C++).
  `services/system/services/` is the consolidated manifest/aggregator dir.
- **`platform/`** — the C++ **runtime** (`platform/runtime/`), the
  **supervisor** (`platform/supervisor/`), and `platform/proto/` (committed
  `.proto` + `.options`; the `.pb.*` are gitignored, genrule-derived).
  Framework-only now — holds `system/services` + `system/supervisor` proto,
  **no `system/apps`** (the demo carries its own proto, see below).
  (The **gateway** moved OUT to the `gataway_ws` consuming workspace.)
- **`demo/`** — the in-repo **consuming workspace** (package **`system.apps`**,
  what `theia init` produces): the app source at
  `demo/system/apps/{package,component}.art`, the generated C++ at
  `demo/apps/Demo3WayP{1..4}/{lib,main,impl}/`, the deploy rigs
  `demo/manifest/{rig,test_rig,zonal_rig}.py`, the demo's OWN proto at
  `demo/proto/system/apps/` (package `system_apps`, distinct from the
  framework's `platform/proto/`), the config-migration example
  `demo/migration/`, the RF tests `demo/tests/`, and its Bazel module
  (`demo/{MODULE.bazel,.bazelrc,.theia,BUILD.bazel}` + shims +
  `system/{supervisor,services,platform/runtime}` symlinks into the
  framework). It binds to the framework via `local_path_override(pero_theia
  = "..")`.
- **`artheia/`** — **submodule** (remote `theia`). The `.art` grammar,
  parser/resolver, code generators, manifest model, LSP, VS Code extension.
  Commit/push it independently of theia, then bump the submodule pin.
- **`tools/`** — `tdb`/`rtdb` (Python debug CLIs over the .art-driven probe),
  `supervisor-gui` (wx observer GUI).
- **`testing/`** — `rf-theia`: Robot Framework harness; self-tests under
  `testing/rf_theia/scenarios/_selftest/`.
- **`rules/`** — Bazel rules (`rig.bzl` module extension, `config/`
  platforms, `deb.bzl` (the `.deb`/`.ipk` packager), `psp.bzl`).
- **`packaging/theia/`** — the ROS2-style `.deb` package set (framework/
  runtime/-dev/services/-dev); see deployment.md.
- **`docs/`** — theia docs (incl. this skill).

## Consuming workspaces (`theia init`) — end to end

A user does **not** edit Theia's own tree to build an app. They scaffold a
SEPARATE **consuming workspace** (the catkin / `colcon` analogue) bound to a
sibling Theia checkout (later, an installed `/opt/theia` prefix). The flow:

```sh
# 0. put a Theia checkout on PATH — exports THEIA_ROOT, prepends its venv +
#    the `theia` launcher, adds artheia to PYTHONPATH.
cd ~/repo/launch-box/gataway_ws        # your (empty) workspace repo
source ../theia/env.sh                 # source-checkout activation (the ROS
                                       # `setup.bash` analogue); the deb ships
                                       # /opt/theia/setup.sh instead

# 1. scaffold this dir as a workspace bound to $THEIA_ROOT.
theia init                             # bare: supervisor + your own apps
#   or:  theia init --with-services    # + the ARA services (com/log/per/sm/ucm/shwa)

# 2. (optional) author an app: add a composition to
#    system/apps/component.art, then generate its C++ + proto:
artheia gen-app --kind fc system/apps/component.art --out apps --proto-out proto

# 3. emit + serialize the manifests, install the runtime layout, run it.
#    `theia init` scaffolds manifest/bootstrap/rig.py — the smoke-test target
#    addressed as `bootstrap`. Real per-target rigs (manifest/single/rig.py, …)
#    come later and are run as `theia manifest <target>`.
artheia gen-manifest system/apps/component.art manifest/apps/manifest.py
theia manifest bootstrap && theia install && theia start
```

What `theia init` creates in the CWD (never overwriting): `system/system.art`
(the workspace aggregator), `system/apps/{package,component}.art` (the
workspace's **own** empty app package, `system.apps` — the REAL, canonical app
source, mapped 1:1 from the FQN: **no `apps/system/apps` indirection, no
symlink**), `manifest/rig.py` (one-machine stub; gen-manifest writes
`manifest/app.py`), `apps/` (home for the generated C++ via `gen-app --out
apps`), `proto/` (home for the generated proto via `gen-app --proto-out
proto`), and a Bazel module (`MODULE.bazel`, `.bazelrc`, `.bazelversion`,
alias-shim `BUILD`s, local proto `BUILD`). It's idempotent — re-run it any
time (e.g. to add `--with-services`). Theia itself is **not vendored** —
`system/platform/runtime`, `system/supervisor` (and, with `--with-services`,
`system/services`) are **relative symlinks into `$THEIA_ROOT`**, so a Theia
bump is a re-source, not a re-copy. `.theia` records the bound `THEIA_ROOT`.

How `theia` resolves work in a consuming workspace (`theia.py`):
- **WORKSPACE** = where you ran `theia` (`$THEIA_INVOCATION_CWD`); **THEIA_ROOT**
  = the framework. The CLI chdir's into THEIA_ROOT for framework verbs but keeps
  WORKSPACE for your targets.
- `_is_framework_target` partitions Bazel labels: `//platform/…` and
  `//services/…` build in **THEIA_ROOT**; your `//apps/…` build in **WORKSPACE**
  against `@pero_theia` (via `local_path_override` + the alias shims).
- `theia start` exports `LD_LIBRARY_PATH`
  (`$THEIA_ROOT/third_party/etcd-cpp-apiv3/install/lib`, `/opt/theia/lib`) so
  `per` finds `libetcd-cpp-api.so`; shutdown is one **batch SIGTERM** of all
  workers reaped against a single deadline (≈0.5 s, not N×timeout).

The in-repo `demo/` is just Theia dogfooding this same path (its rig is
`demo/manifest/rig.py`) so CI exercises the consuming flow against the real
framework.

## The Functional Cluster catalog

The 18 AUTOSAR FCs are enumerated in
`artheia/artheia/manifest/clusters.py` (`CLUSTERS` / `BY_SHORT`). Six ship a
real daemon today; the rest are placeholders carrying a TIPC slot + the
AUTOSAR-named interface contract but no process:

| short | what | daemon? |
| --- | --- | --- |
| `com` | Communication Mgmt — the gRPC bridge (supervisor ↔ external tools), two-codec proxy | ✅ |
| `log` | Log & Trace — logs go **direct to the sink**; `log[trace]` is a logcat-style ring-buffer **trace hub** (observers `Subscribe` over TIPC) | ✅ |
| `per` | Persistency | ✅ |
| `sm` | State Management — a `GenStateM` FSM | ✅ |
| `ucm` | Update & Config Mgmt | ✅ |
| `shwa` | Safe Hardware Accelerator + host monitor (runs on every machine) | ✅ |
| `exec` | Execution Mgmt — **realized BY the supervisor**, not a separate process | ⛔ (nop) |
| `core` `crypto` `diag` `fw` `idsm` `nm` `osi` `phm` `rds` `tsync` `vucm` | AUTOSAR placeholders | ⛔ (nop) |


## The Theia C++ runtime

`platform/runtime/include/` — an OTP-faithful actor runtime. One **thread per node**. Three node base classes (picked by the `.art` node shape):

- **`GenServer`** — sync-first mailbox actor: `handle_call` (request→reply),
  `handle_cast` (fire-and-forget), `handle_info` (unrouted TIPC frame /
  internal signal; default is CRITICAL+abort — drift is a bug).
- **`GenStateM`** — finite state machine (Erlang `gen_statem` shape);
  per-state event handlers + timeouts.
- **`GenRunnable`** — a free worker thread: `do_start` / `do_loop` /
  `do_stop`.

Transport is **TIPC** (`TipcMux`, per-process listener) carrying **nanopb**
wire bytes; `RemoteRef`/`NodeRef` do correlated request/reply;
`RemoteCodec` hashes a stable 16-bit `service_id` per message type.
`Tracer` emits `[header][proto-wire]` records when a node is selectively
enabled. The host-side **trace decoder is pluggable**: the framework ships
`//platform/runtime/trace:libtrace_decoder_system.so` (system_services_sm_*
types, in the deb) and each app workspace ships its own (the demo's
`//trace:libtrace_decoder_apps.so`, system_apps_* types). Consumers
(supervisor-gui, rtdb, rf-theia) dlopen **every** `libtrace_decoder_*.so` from
the plugin dir (`THEIA_TRACE_DECODER_PATH`, exported by `env.sh` in source /
the deb's `setup.sh`) and try
each.

## The supervisor

`platform/supervisor/` — a fork/exec orchestrator (one Process
per FC binary + per vendor app), owning Function-Group state. It exposes a
**control surface** (`RestartChild`/`TerminateChild`/`StartChild`/
`DeleteChild`, `ConfigureTrace`, `ConfigureLogLevel`) as a `gen_server`
node on the standard Theia transport (nanopb `ControlRequest`), runs a
**heartbeat watchdog**.

The `com` FC bridges theia↔gRPC to external tools (`supdbg`, `supervisor-gui`, `rf-theia`).

## The pipeline, end to end

```
 .art system tree           gen-app --kind fc      C++ FC daemons
 (system/, services/)  ──►   per node:        ──►  //services/<fc>/main:<fc>
        │                    lib/ main/ impl/
        │
        │ load_platform_services (system/services as root)
        ▼
 manifest python layer       executor emit /        serialized JSON manifests
 (services/manifest,    ──►  generate-manifest  ──► dist/manifest/<machine>/*.json
  demo/manifest/rig)
        │
        │ rig_ext (//rules:rig.bzl)  /  `theia manifest` + `theia install`
        ▼
 bazel build @rig_apps//…  ──►  bundles, install/ runtime layout
```

`@rig_apps` (and `@rig_test` / `@rig_zonal`) are declared in the **workspace's**
`MODULE.bazel` now (via `@pero_theia//rules:rig.bzl`), not the framework root.
In a **consuming workspace** the same pipeline runs against the user's own
rig (`manifest/rig.py` in the workspace; `demo/manifest/rig.py` for the demo)
and `theia` resolves framework targets against `$THEIA_ROOT` — see "Consuming
workspaces" above.

Details and exact commands are in
[references/artheia-gen-system.md](references/artheia-gen-system.md).

## Build & verify (cheat sheet)

```sh
artheia parse system/system.art                 # full-tree resolve (validation)
bazel build --config=linux //services/com/main:com //services/sm/main:sm …
bazel build @rig_apps//:executor_json           # the manifest pipeline under Bazel
bazel build //system:art_sources                # the .art filegroup

# Regen-stability guard — committed lib/main/impl/BUILD must equal gen-app output:
robot rf_theia/scenarios/_selftest/fc_regen_stability/fc_regen_stability.robot
```
Do **not** commit `MODULE.bazel.lock`.

## MCP tools (dev-loop as an API)

The repo's `.mcp.json` wires four MCP servers (each `run_mcp.sh` sources the
workspace venv). Prefer these over shelling out when a tool fits — they run the
same code in-process:

- **`artheia`** — the generators + validators as tools: `parse`,
  `check_addresses`, `gen_app`, `gen_manifest`, `gen_proto`, `gen_schema`,
  `gen_netgraph`, `list_generators`, `describe`. Paths resolve against the
  workspace root.
- **`theia`** — the dev loop + live inspect: `theia_init`, `theia_manifest`,
  `theia_install`, `theia_start`/`theia_stop`, `theia_call`/`theia_cast`;
  `tdb_apps`/`tdb_ps`/`tdb_trace` (local supervisor over the probe),
  `rtdb_machines`/`rtdb_ps` (remote board over com), `colony_*` (fleet deploy).
  Release/publish verbs are intentionally NOT exposed (human-gated).
- **`rf-theia`** — the Robot Framework harness: `list_scenarios`,
  `run_scenario`, `list_keywords`, `get_test_results`, `analyze_trace`,
  `tail_supervisor_log`.
- **`work-with-me`** — the review/checkpoint helpers (`check_me`, `compare_me`,
  …).

**Workspace binding gotcha.** The `theia`/`artheia`/`rf-theia` servers fix their
workspace at startup to `THEIA_INVOCATION_CWD` (the dir Claude Code launched
from, = the `.mcp.json` dir) — there's NO per-call workspace argument. So the
tools operate on THAT workspace only. `theia init` does NOT scaffold a
workspace-local `.mcp.json`, so a session in a *consuming* workspace has no
theia MCP server unless one is added there. When the session root isn't the
workspace you mean to act on, fall back to the shell CLI (`theia …` / `artheia
…`) with the right cwd. The `tdb_*` inspect tools additionally need a LIVE
supervisor — they return a clean `ConnectionError` / `(empty tree)` when none
is running.

## Conventions worth knowing

- **Generated files are not hand-edited.** `lib/`, `main/`, and `BUILD`
  carry an `AUTO-GENERATED … DO NOT EDIT` banner. The **app-owned,
  write-once** files are BOTH `impl/<Node>_handlers.cc` (handler bodies)
  **and** `impl/<Node>_state.hh` (the node's state struct) — gen-app
  scaffolds them once and never overwrites them (only `--force` clobbers,
  and it clobbers both). To change generated output, change the `.art` (or
  the template in `artheia/.../templates/fc_app/`) and re-run gen-app. The
  regen-stability test enforces this.
- **nanopb `.options`** are emitted by gen-app alongside the proto (pinning
  string/bytes fields to fixed `char[]`); a `.art` field can size its buffer
  with `[max_size:N]`. Committed under `platform/proto/system/<pkg>/`.
- **Cross-repo Bazel labels are hierarchical**: `//gateway/pero_cmp_lnx/lib:…`,
  never `//pero_cmp_lnx/lib:…`.
- **Branch convention**: the manifest pins `main` everywhere; do work on a
  feature branch, FF-merge into `main`, then push + open a PR.
- **artheia is a separate (sub)repo** — commit and push it on its own, then
  bump the submodule pin in theia. Never commit `MODULE.bazel.lock`.

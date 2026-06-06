---
name: theia
description: Refresher for agents working in the theia monorepo — the AUTOSAR-Adaptive-style platform (Functional Clusters on the Theia C++ actor runtime, modeled in the artheia .art DSL). Orients you to the layout, the build, and where to dig deeper. Args: (none) — read top-to-bottom, then load a reference under references/ for the task at hand.
disable-model-invocation: true
---

Theia is a AUTOSAR-Adaptive-Platform-style
**Functional Clusters (FCs)** running as supervised processes on a custom
C++ **actor runtime** ("the Theia runtime"), with the whole system modeled
in the **artheia** `.art` DSL and built with Bazel inside a Google-`repo`
workspace.

Read this page to orient. For a specific task, load the matching reference:

| You're doing… | Read |
| --- | --- |
| **Removing / simplifying / rewriting code** (read first) | [references/requirements.md](references/requirements.md) — functional vs non-functional |
| Writing or editing `.art` files | [references/art-lang-grammar.md](references/art-lang-grammar.md) |
| Generating / regenerating an application C++ (lib/main/impl) | [references/artheia-gen-app.md](references/artheia-gen-app.md) |
| Regenerating the system: .art → manifests (stages 1–2) | [references/artheia-gen-system.md](references/artheia-gen-system.md) |
| Bazel build: rig extension, FC daemons, `.ipk` bundles | [references/build-system.md](references/build-system.md) |
| Provisioning & orchestration: Puppet, docker compose, Pi 4 push | [references/provision-orchestrate.md](references/provision-orchestrate.md) |
| AUTOSAR PSP import: DBC/FIBEX → `.art` + netgraph | [references/autosar.md](references/autosar.md) |
| Config schema migration: `config <Msg>` evolution, transform.json, gen-transform plugin | [references/migration.md](references/migration.md) |

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

The repo is a Google-`repo` workspace; several dirs are sibling git repos
checked out by `repo sync` (gitignored in pero_theia). Major dirs:

- **`system/`** — the **virtual root** of the `system` package and the
  aggregation point everything resolves against. `system/system.art`
  (`cluster Platform`), `system/services/cluster.art` (`cluster Services`),
  and per-FC symlinks `system/services/<fc>` → the real spec in the impl
  tree.
- **`services/`** — the FCs. 6 with real daemons live at
  `services/<fc>/system/<fc>/{package,component}.art` (spec) +
  `services/<fc>/{lib,main,impl}/` (generated + hand-owned C++).
- **`platform/`** — the C++ **runtime** (`platform/runtime/`), the
  **supervisor** (`platform/supervisor/`), the **gateway** service
  (`platform/gateway/`, a sibling repo), and `platform/config/`,
  `platform/proto/` (committed `.proto`; the `.pb.*` are gitignored,
  genrule-derived).
- **`artheia/`** — **separate git repo** (remote `theia`). The `.art`
  grammar, parser/resolver, code generators, manifest model, LSP, VS Code
  extension. Commit/push it independently of pero_theia.
- **`demo/`** — the demo rig (`demo/manifest/rig.py` = `Demo3Way`) +
  `demo/system/demo/` app `.art`.
- **`tools/`** — `supdbg` (Python gRPC debug CLI), `supervisor-gui`
  (wx observer GUI).
- **`testing/`** — `rf-theia`: Robot Framework + TPT harness.
- **`rules/`** — Bazel rules (`rig.bzl` module extension, `config/`
  platforms, `psp.bzl`, `opkg.bzl`).
- **`autosar/`** — the vendor PSP (`mlbevo_gen2_cmp_psp`): FIBEX/DBC →
  catalog/`.art`/netgraph. **`gateway/`** — CMP codec libs + Hercules
  firmware (sibling repos). **`vendor/`** — per-vendor app fragments.
- **`docs/`** — theia docs.

## The Functional Cluster catalog

The 18 AUTOSAR FCs are enumerated in
`artheia/artheia/manifest/clusters.py` (`CLUSTERS` / `BY_SHORT`). Six ship a
real daemon today; the rest are placeholders carrying a TIPC slot + the
AUTOSAR-named interface contract but no process:

| short | what | daemon? |
| --- | --- | --- |
| `com` | Communication Mgmt — the gRPC bridge (supervisor ↔ external tools), two-codec proxy | ✅ |
| `log` | Log & Trace — two nodes: `LogDaemon` (syslog sink) + `TraceCollector` (trace fan-out) | ✅ |
| `per` | Persistency | ✅ |
| `sm` | State Management — a `GenStateM` FSM | ✅ |
| `ucm` | Update & Config Mgmt | ✅ |
| `shwa` | Safe Hardware Accelerator (pinned to the compute machine) | ✅ |
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
enabled.

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
        │ rig_ext (//rules:rig.bzl)
        ▼
 bazel build @rig_demo//…  ──►  .ipk bundles, //:install runtime layout
```

Details and exact commands are in
[references/artheia-gen-system.md](references/artheia-gen-system.md).

## Build & verify (cheat sheet)

```sh
artheia parse system/system.art                 # full-tree resolve (validation)
bazel build --config=linux //services/com/main:com //services/sm/main:sm …
bazel build @rig_demo//:executor_json           # the manifest pipeline under Bazel
bazel build //system:art_sources                # the .art filegroup

# Regen-stability guard — committed lib/main/impl/BUILD must equal gen-app output:
robot rf_theia/scenarios/_selftest/fc_regen_stability/fc_regen_stability.robot
```
Do **not** commit `MODULE.bazel.lock`.

## Conventions worth knowing

- **Generated files are not hand-edited.** `lib/` and `main/` carry an
  `AUTO-GENERATED … DO NOT EDIT` banner; only `impl/<Node>_handlers.cc`
  (`FIRST-TIME-ONLY SCAFFOLD`) is yours. To change generated output, change
  the `.art` (or the template in `artheia/.../templates/fc_app/`) and
  re-run gen-app. The regen-stability test enforces this.
- **Cross-repo Bazel labels are hierarchical**: `//gateway/pero_cmp_lnx/lib:…`,
  never `//pero_cmp_lnx/lib:…`.
- **Branch convention**: the manifest pins `main` everywhere; do work on a
  feature branch, FF-merge into `main`, then push. **GitLab**
  (not Gerrit): `git push theia HEAD:<branch>` + MR.
- **artheia is a separate repo** — commit and push it on its own.

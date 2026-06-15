# Theia (`pero_theia`)

An AUTOSAR-Adaptive-Platform-style framework: **Functional Clusters (FCs)**
running as supervised processes on a custom C++ **actor runtime** ("the Theia
runtime"), with the whole system modeled in the **artheia** `.art` DSL and built
with Bazel.

It's a **standalone git repo** (departed Google-`repo`) with a small set of
git submodules. You author your apps in a SEPARATE **consuming workspace**
scaffolded by `theia init`; the in-repo `apps/` is Theia's own demo, exercising
the framework in CI.

## Repositories

| Repo | GitLab (origin) | GitHub mirror | Role |
| --- | --- | --- | --- |
| **pero_theia** | `PG50/pero_theia` | `perotheia/theia` | this repo — runtime, supervisor, services, build/packaging |
| **artheia** | `PG50/artheia` | `perotheia/artheia` | submodule — the `.art` DSL, generators, LSP |
| **rf-theia** | `PG50/rf-theia` | `perotheia/rf-theia` | submodule — the Robot Framework testing harness (library only) |
| **docs** | `PG50/docs` | *(GitLab only)* | a plain clone at `docs/` (gitignored here, not mirrored) |

Submodules also include `third_party/etcd-cpp-apiv3`. Each repo has a `github`
remote alongside `origin`; push to both to keep the mirror current.

## Layout

```
artheia/            submodule — the artheia DSL + code generators (pip install -e)
platform/
  runtime/          the C++ actor runtime (GenServer/GenStateM/GenRunnable, TipcMux…)
  supervisor/       the OTP-style fork/exec supervisor (Execution Management)
  proto/            committed .proto + .options; .pb.* are genrule-derived
services/           the ARA Functional Clusters (com, per, sm, ucm, log, shwa, …)
apps/               Theia's own demo app (package system.apps) + deploy rigs
system/             the virtual root the .art tree resolves against
rf-theia/           submodule — the RF testing harness (TheiaTestLibrary + MCP)
testing/scenarios/  the .robot test scenarios (project tests; import rf_theia)
contrib/
  editors/          VS Code + Emacs LSP clients for .art
  skills/           Claude Code agent skills
packaging/theia/    the ROS2-style .deb package set (framework/runtime/services)
rules/, toolchains/ Bazel rules (rig.bzl, deb.bzl, config/) + cross-compile cfg
docs/               a plain clone of the docs repo (gitignored — see above)
```

The three primitives everything in `.art` rests on:

| Primitive | Is a | Owns |
| --- | --- | --- |
| **node** | thread | one TIPC `type/instance`, ports, a `GenServer`/`GenStateM`/`GenRunnable` |
| **composition** | process (one executable) | node instances + in-process wiring |
| **cluster** | distribution bundle | compositions + inter-process wiring; the deploy/packaging unit |

## Quick start (developing in this repo)

```sh
git clone --recurse-submodules ssh://git@cicd.skyway.porsche.com/PG50/pero_theia
cd pero_theia
python3 -m venv .venv && . .venv/bin/activate
pip install -e artheia/ -e . -e 'rf-theia/[mcp]'   # artheia + the workspace + harness

source setup.sh                                    # THEIA_ROOT, PATH, PYTHONPATH
artheia parse system/system.art                    # validate the full .art tree
PATH="$PWD/.venv/bin:$PATH" bazel build //services/...:all
theia manifest && theia install && theia start     # bring the stack up
```

## Consuming workspaces (`theia init`)

A user does NOT edit this repo to build an app. They scaffold their **own**
workspace against a Theia checkout (or an installed `/opt/theia`):

```sh
cd ~/my_ws
python3 -m venv .venv && . .venv/bin/activate
pip install --find-links /opt/theia/wheels artheia rf-theia   # from the deb's wheels
source /opt/theia/setup.sh
theia init [--with-services]                       # scaffold against $THEIA_ROOT
artheia gen-app --kind fc apps/system/apps/component.art --out apps --proto-out platform/proto
theia manifest && theia install && theia start
```

See [`contrib/skills/theia/SKILL.md`](contrib/skills/theia/SKILL.md) for the
full orientation, and the **docs** repo (cloned at `docs/`) for the manuals.

## Editor support

`.art` gets syntax highlighting + an LSP (diagnostics, goto-definition,
completion) in **VS Code** and **Emacs** — see
[`contrib/editors/`](contrib/editors/README.md). Both drive the same
`artheia-lsp` server that ships with the artheia package.

## Testing

The Robot Framework harness is the `rf-theia` submodule (a reusable library);
the `.robot` scenarios that test *this* Theia live in `testing/scenarios/` and
import the harness as `rf_theia.TheiaTestLibrary`. Run via `robot` or the
`rf-theia` MCP server (`rf-theia/run_mcp.sh`, wired in `.mcp.json`).

## Packaging

`theia release` builds the installable `.deb` set (framework / runtime / -dev /
services / -dev) under `dist/debian/`. The framework deb ships artheia + rf-theia
as **wheels** under `/opt/theia/wheels` (the user installs them into their own
venv) and makes `/opt/theia` a consumable `pero_theia` Bazel module. See
[`docs/`](docs/) → deployment for the full split.

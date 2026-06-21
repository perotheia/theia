*** Settings ***
Documentation    End-to-end selftest of the Demo3Way generation
...              pipeline. One test case per stage so a regression
...              points at the first broken hop instead of a generic
...              "the demo build broke" failure mode.
...
...              This is a DEMO-APP test: it runs FROM the demo
...              consuming workspace (demo/), driving artheia against
...              the demo's own `system/apps/component.art` +
...              `manifest.rig`. The demo rig is single-machine — every
...              component lands on `central` (x86_64/amd64). The
...              multi-host split lives in manifest/zonal_rig.py and is
...              exercised by the two_supervisor selftest, not here.
...
...              Pipeline (matches docs/architecture.md):
...
...                .art source  ──┐
...                rig.py        ─┤  artheia parse / rig-deps / gen-netgraph
...                               │  gen-routing / gen-app
...                               │
...                              .json + .hh + .cc + BUILD.bazel
...                               │
...                               │  artheia serialize-manifest
...                               │  (validate → per-machine JSON, incl.
...                               │   the executor.json supervisor slice)
...                               │
...                              dist/manifest/<machine>/*.json
...                               │
...                               │  bazel build @rig_apps//<m>:image
...                               │  (rules/rig.bzl pkg_opkg)
...                               ↓
...                              demo-<machine>_1.0.0_<arch>.ipk
...
...              Stages 1–7 run on a fresh /tmp workdir — no commit
...              footprint, no order dependence between cases past
...              the suite setup. Stage 8 is gated 'live-bazel' so
...              CI can skip when bazel isn't available.
...
...              Why one keyword per stage rather than one big
...              script: when stage N changes shape (rename, schema
...              tweak), only stage N's assertions update. The lib's
...              public surface is small and stable.
Library          ${CURDIR}/demo_chain_lib.py
Library          OperatingSystem


Suite Setup      Set Up Workspace + Workdir


*** Variables ***
${TMPDIR}        %{TMPDIR=/tmp}
${WORKDIR}       ${TMPDIR}/demo_chain_selftest
# Workspace root — this suite lives at demo/tests/demo_chain/, so the
# demo consuming-workspace root (the dir with MODULE.bazel +
# system/apps/component.art + manifest/) is two levels up.
${WORKSPACE}     ${CURDIR}/../..


*** Keywords ***
Set Up Workspace + Workdir
    # Resolve to a clean absolute path so the lib can chdir into it.
    ${ws_abs}=    Evaluate    str(__import__('pathlib').Path(r"${WORKSPACE}").resolve())
    Use Workspace    ${ws_abs}
    Remove Directory    ${WORKDIR}    recursive=${TRUE}
    Use Workdir         ${WORKDIR}


*** Test Cases ***
Stage 1 — parse component.art
    [Documentation]    `artheia parse system/apps/component.art`
    ...                must succeed and dump a tree that mentions
    ...                the Demo3Way cluster + its process
    ...                compositions.
    [Tags]    demo-chain    hermetic    selftest    stage-1

    ${tree}=    Stage 1 Parse Component Art
    Tree Mentions    ${tree}    Demo3Way
    Tree Mentions    ${tree}    Demo3WayP1
    Tree Mentions    ${tree}    Demo3WayP2
    Tree Mentions    ${tree}    Demo3WayP3


Stage 2 — rig-deps JSON
    [Documentation]    `artheia rig-deps manifest.rig` emits the
    ...                Bazel rig_ext extension's input. The demo rig is
    ...                single-machine: every component pins to `central`
    ...                (x86_64 → amd64). Asserts the machine + arch and
    ...                that p1/p2/p3 land on it.
    [Tags]    demo-chain    hermetic    selftest    stage-2

    ${rig_json}=    Stage 2 Rig Deps
    Json Has Machines    ${rig_json}    central
    Machine Arch Is      ${rig_json}    central    amd64
    Json Has Component   ${rig_json}    p1    central
    Json Has Component   ${rig_json}    p2    central
    Json Has Component   ${rig_json}    p3    central


Stage 3 — gen-netgraph JSON
    [Documentation]    Netgraph JSON describing nodes + cluster
    ...                routing. Driver/Ticker/Counter/Observer/
    ...                Incrementer are the canonical Demo3Way nodes;
    ...                if any drop out the cluster wiring broke.
    [Tags]    demo-chain    hermetic    selftest    stage-3

    ${netgraph}=    Stage 3 Gen Netgraph
    Netgraph Has Nodes    ${netgraph}    DriverNode    TickerNode    CounterNode
    Netgraph Has Nodes    ${netgraph}    ObserverNode  IncrementerNode


Stage 4 — gen-routing per-process headers (KNOWN BROKEN, #378)
    [Documentation]    Each composition (P1/P2/P3) gets a
    ...                Demo3Way__<P>_refs.hh with LocalRef +
    ...                RemoteRef wiring. The header is what the
    ...                hand-written node main.cc binds against.
    ...
    ...                Currently broken: gen-routing uses raw
    ...                textx.metamodel_from_file and doesn't resolve
    ...                sibling .art files in the same package — fails
    ...                with `Unknown object "CounterNode"`. Tracked
    ...                in task #378. Test asserts the EXPECTED-FAIL
    ...                error message so we get a green when the bug
    ...                is fixed and the assertion swaps to "header
    ...                exists".
    [Tags]    demo-chain    hermetic    selftest    stage-4
    ...      known-broken

    Run Keyword And Expect Error    *Unknown object "CounterNode"*
    ...    Stage 4 Gen Routing    Demo3WayP1


Stage 5 — gen-app per-process skeleton
    [Documentation]    One app skeleton per `on process P` partition,
    ...                emitted by `gen-app --kind fc --composition`.
    ...                gen-app appends the composition to --out, so the
    ...                project lands at <out>/Demo3WayP1/{lib,main,impl}.
    [Tags]    demo-chain    hermetic    selftest    stage-5

    ${out}=    Stage 5 Gen App Composition    Demo3WayP1
    App Composition Has Process Dir    ${out}    Demo3WayP1


Stage 6 — serialize-manifest JSON set
    [Documentation]    Per-machine deploy manifest set — 5 JSON files
    ...                + a top-level machines.json index, emitted by the
    ...                orthogonal-engine `serialize-manifest` (which
    ...                folded the old generate-manifest + executor emit
    ...                into one validate-then-write command). The C++
    ...                supervisor + supervisor-gui read from here
    ...                directly via nlohmann/json (no yaml-cpp dep
    ...                since #380). Asserts p1/p2/p3 land in central's
    ...                execution.json.
    [Tags]    demo-chain    hermetic    selftest    stage-6

    ${root}=    Stage 6 Serialize Manifest
    Manifest Has Machine Jsons    ${root}    central

    Execution Json Lists Process    ${root}    central    p1
    Execution Json Lists Process    ${root}    central    p2
    Execution Json Lists Process    ${root}    central    p3


Stage 6b — manifest schema sanity (#379, #380)
    [Documentation]    Each JSON file carries its defining top-level
    ...                container key — the orthogonal-engine serializer
    ...                emits the bare payload (no `kind` discriminator),
    ...                so a file's identity IS its top key
    ...                (execution→processes, …). executor.json is the
    ...                NESTED supervisor tree: its root object is the
    ...                `root` supervisor (name + children), the shape the
    ...                C++ supervisor parses. Plus: no YAML (#380).
    [Tags]    demo-chain    hermetic    selftest    stage-6b

    ${root}=    Stage 6 Serialize Manifest

    # Top-level container keys (one structural identity per file).
    Json Top Key Is    ${root}    central    machine        name
    Json Top Key Is    ${root}    central    application    applications
    Json Top Key Is    ${root}    central    service        instances
    Json Top Key Is    ${root}    central    execution      processes
    # executor.json is the nested tree — its root carries `children`.
    Json Top Key Is    ${root}    central    executor       children

    # machines.json — the machine-name list the deploy bootstrap reads.
    Index Json Lists Machines    ${root}    central

    # Hard wall against YAML re-emergence.
    No Yaml Emitted    ${root}


Stage 7 — per-machine supervisor tree (executor.json slice)
    [Documentation]    Per-machine supervisor tree — the NESTED tree the
    ...                C++ supervisor forks against (root one_for_all,
    ...                children are nested supervisors or fully-populated
    ...                worker leaves). serialize-manifest writes it as the
    ...                <machine>/executor.json slice. Each worker leaf
    ...                carries its .art nodes (tipc) from PROCESS_NODES —
    ...                p1 hosts counter/driver/ticker.
    [Tags]    demo-chain    hermetic    selftest    stage-7

    ${root}=    Stage 6 Serialize Manifest
    ${y}=    Executor Slice For Machine    ${root}    central
    Executor Json Root Strategy Is    ${y}    one_for_all
    Executor Worker Has Nodes    ${y}    p1    counter    driver    ticker


Stage 8 — bazel build .ipk image (arch tag matches rig.py)
    [Documentation]    The terminal stage — bazel build produces
    ...                an .ipk whose arch tag matches what rig.py
    ...                declared for the machine (#371). The demo's
    ...                single `central` machine declared x86_64 →
    ...                _amd64.ipk.
    ...
    ...                Tagged 'live-bazel' — skip in CI environments
    ...                where Bazel isn't installed.
    [Tags]    demo-chain    live-bazel    selftest    stage-8

    ${ipk}=    Stage 8 Bazel Build Image    central
    Ipk Name Carries Arch    ${ipk}    amd64

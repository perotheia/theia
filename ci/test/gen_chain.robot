*** Settings ***
Documentation    End-to-end test of the .art→.ipk generation
...              pipeline. One test case per stage so a regression
...              points at the first broken hop instead of a generic
...              "the build broke" failure mode.
...
...              It runs FROM a CONSUMING workspace — in the harness,
...              the fresh scaffold ci/run.sh s2 builds (Demo3Way seed
...              grafted), passed in as ${WS}. The scaffold's bootstrap
...              rig is single-machine — every component lands on
...              `central` (x86_64/amd64).
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
...                              theia-<machine>_1.0.0_<arch>.ipk
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
Library          ${CURDIR}/gen_chain_lib.py
Library          OperatingSystem


Suite Setup      Set Up Workspace + Workdir


*** Variables ***
${TMPDIR}        %{TMPDIR=/tmp}
${WORKDIR}       ${TMPDIR}/gen_chain_selftest
# Consuming-workspace root (the dir with MODULE.bazel +
# system/apps/component.art + manifest/) — the harness passes the fresh
# s2 scaffold via --variable WORKSPACE:...; no committed default exists.
${WORKSPACE}     ${EMPTY}
${RIG}           manifest.apps.rig
${RIG_REPO}      rig_apps


*** Keywords ***
Set Up Workspace + Workdir
    Should Not Be Empty    ${WORKSPACE}    pass the workspace: --variable WORKSPACE:<ws>
    # Resolve to a clean absolute path so the lib can chdir into it.
    ${ws_abs}=    Evaluate    str(__import__('pathlib').Path(r"${WORKSPACE}").resolve())
    Use Workspace    ${ws_abs}
    Use Rig          ${RIG}    RIG    ${RIG_REPO}
    Remove Directory    ${WORKDIR}    recursive=${TRUE}
    Use Workdir         ${WORKDIR}


*** Test Cases ***
Stage 1 — parse component.art
    [Documentation]    `artheia parse system/apps/component.art`
    ...                must succeed and dump a tree that mentions
    ...                the Demo3Way cluster + its process
    ...                compositions (the seed apps).
    [Tags]    gen-chain    hermetic    selftest    stage-1

    ${tree}=    Stage 1 Parse Component Art
    Tree Mentions    ${tree}    Demo3Way
    Tree Mentions    ${tree}    Demo3WayP1
    Tree Mentions    ${tree}    Demo3WayP2
    Tree Mentions    ${tree}    Demo3WayP3


Stage 2 — rig-deps JSON
    [Documentation]    `artheia rig-deps ${RIG}` emits the
    ...                Bazel rig_ext extension's input. The bootstrap rig
    ...                is single-machine: every component pins to `central`
    ...                (x86_64 → amd64). Asserts the machine + arch and
    ...                that p1/p2/p3 land on it.
    [Tags]    gen-chain    hermetic    selftest    stage-2

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
    [Tags]    gen-chain    hermetic    selftest    stage-3

    ${netgraph}=    Stage 3 Gen Netgraph
    Netgraph Has Nodes    ${netgraph}    DriverNode    TickerNode    CounterNode
    Netgraph Has Nodes    ${netgraph}    ObserverNode  IncrementerNode


Stage 4 — gen-routing per-process headers
    [Documentation]    Each composition gets a <Comp>__<P>_refs.hh
    ...                with LocalRef + RemoteRef wiring. The header is
    ...                what a hand-written node main.cc binds against.
    ...
    ...                Was #378 (KNOWN BROKEN): gen-routing used raw
    ...                textx.metamodel_from_file and couldn't resolve
    ...                sibling .art files in the same package — died
    ...                with `Unknown object "CounterNode"`. Fixed by
    ...                switching to the canonical parse_file loader
    ...                (sibling merge + imports + inheritance); this
    ...                case now asserts the header actually emits.
    [Tags]    gen-chain    hermetic    selftest    stage-4

    ${out}=    Stage 4 Gen Routing    Demo3WayP1
    Routing Header Exists    ${out}    Demo3WayP1    P1


Stage 5 — gen-app per-process skeleton
    [Documentation]    One app skeleton per `on process P` partition,
    ...                emitted by `gen-app --kind fc --composition`.
    ...                gen-app appends the composition to --out, so the
    ...                project lands at <out>/Demo3WayP1/{lib,main,impl}.
    [Tags]    gen-chain    hermetic    selftest    stage-5

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
    [Tags]    gen-chain    hermetic    selftest    stage-6

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
    [Tags]    gen-chain    hermetic    selftest    stage-6b

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
    [Tags]    gen-chain    hermetic    selftest    stage-7

    ${root}=    Stage 6 Serialize Manifest
    ${y}=    Executor Slice For Machine    ${root}    central
    Executor Json Root Strategy Is    ${y}    one_for_all
    Executor Worker Has Nodes    ${y}    p1    counter    driver    ticker


Stage 8 — bazel build .ipk image (arch tag matches rig.py)
    [Documentation]    The terminal stage — bazel build produces
    ...                an .ipk whose arch tag matches what rig.py
    ...                declared for the machine (#371). Arch tags are
    ...                format-specific (rules/pack_ipk.py): .deb wants
    ...                amd64/arm64, .ipk (opkg) wants x86_64/aarch64 —
    ...                so the bootstrap rig's x86_64 machine yields
    ...                _x86_64.ipk. (The retired demo/ copy of this
    ...                suite still asserted the pre-split _amd64 tag —
    ...                its stage 8 never ran in CI, so it rotted.)
    ...
    ...                Tagged 'live-bazel' — skip in CI environments
    ...                where Bazel isn't installed.
    [Tags]    gen-chain    live-bazel    selftest    stage-8

    ${ipk}=    Stage 8 Bazel Build Image    central
    Ipk Name Carries Arch    ${ipk}    x86_64

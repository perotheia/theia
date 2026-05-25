*** Settings ***
Documentation    End-to-end selftest of the Demo3Way generation
...              pipeline. One test case per stage so a regression
...              points at the first broken hop instead of a generic
...              "the demo build broke" failure mode.
...
...              Pipeline (matches docs/architecture.md):
...
...                .art source  ──┐
...                rig.py        ─┤  artheia parse / rig-deps / gen-netgraph
...                               │  gen-routing / gen-app-composition
...                               │
...                              .yaml + .json + .hh + .cc + CMakeLists
...                               │
...                               │  artheia generate-manifest
...                               │  artheia executor emit
...                               │
...                              dist/manifest/<machine>/*.yaml
...                               │
...                               │  bazel build @rig_demo//<m>:image
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
# Workspace root — when run from `testing/` with PYTHONPATH=. (the
# canonical TESTING.md command), CURDIR resolves under testing/.
# We need the pero_theia checkout one level up.
${WORKSPACE}     ${CURDIR}/../../../../..


*** Keywords ***
Set Up Workspace + Workdir
    # Resolve to a clean absolute path so the lib can chdir into it.
    ${ws_abs}=    Evaluate    str(__import__('pathlib').Path(r"${WORKSPACE}").resolve())
    Use Workspace    ${ws_abs}
    Remove Directory    ${WORKDIR}    recursive=${TRUE}
    Use Workdir         ${WORKDIR}


*** Test Cases ***
Stage 1 — parse component.art
    [Documentation]    `artheia parse demo/system/demo/component.art`
    ...                must succeed and dump a tree that mentions
    ...                the Demo3Way cluster + its 3 process
    ...                compositions.
    [Tags]    demo-chain    hermetic    selftest    stage-1

    ${tree}=    Stage 1 Parse Component Art
    Tree Mentions    ${tree}    Demo3Way
    Tree Mentions    ${tree}    Demo3WayP1
    Tree Mentions    ${tree}    Demo3WayP2
    Tree Mentions    ${tree}    Demo3WayP3


Stage 2 — rig-deps JSON
    [Documentation]    `artheia rig-deps demo.manifest.rig` emits
    ...                the Bazel rig_ext extension's input.
    ...                Asserts machine list, per-machine arch, and
    ...                that demo_p1/p2/p3 components pin to
    ...                compute_host (per #289-style pinning).
    [Tags]    demo-chain    hermetic    selftest    stage-2

    ${rig_json}=    Stage 2 Rig Deps
    Json Has Machines    ${rig_json}    admin_host    central_host    compute_host
    # ComputeHost is aarch64 in rig.py — #371 wired this through.
    Machine Arch Is      ${rig_json}    compute_host    arm64
    Machine Arch Is      ${rig_json}    central_host   amd64
    Json Has Component   ${rig_json}    demo_p1    compute_host
    Json Has Component   ${rig_json}    demo_p2    compute_host
    Json Has Component   ${rig_json}    demo_p3    compute_host


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


Stage 5 — gen-app-composition CMake projects (KNOWN BROKEN, #378)
    [Documentation]    One CMake project per `on process P`
    ...                partition. Today these are still CMake (not
    ...                yet cc_binary); regression here would mean
    ...                the codegen path that ties refs.hh to a real
    ...                build broke.
    ...
    ...                Same broken loader as stage 4 — see #378.
    [Tags]    demo-chain    hermetic    selftest    stage-5
    ...      known-broken

    Run Keyword And Expect Error    *Unknown object "CounterNode"*
    ...    Stage 5 Gen App Composition    Demo3WayP1


Stage 6 — generate-manifest YAML set
    [Documentation]    Per-machine deploy manifest set — 4 YAMLs +
    ...                index.yaml. The supervisor + Puppet reads
    ...                from here. Asserts demo_p1/p2/p3 land in
    ...                compute_host's execution.yaml.
    [Tags]    demo-chain    hermetic    selftest    stage-6

    ${root}=    Stage 6 Generate Manifest
    Manifest Has Machine Yamls    ${root}    central_host
    Manifest Has Machine Yamls    ${root}    compute_host
    Manifest Has Machine Yamls    ${root}    admin_host

    Execution Yaml Lists Process    ${root}    compute_host    demo_p1
    Execution Yaml Lists Process    ${root}    compute_host    demo_p2
    Execution Yaml Lists Process    ${root}    compute_host    demo_p3


Stage 6b — manifest JSON siblings (#379)
    [Documentation]    Every YAML manifest has a JSON sibling with
    ...                identical content. JSON is the
    ...                programmatic-tooling encoding (linters,
    ...                schema validators, downstream codegen that
    ...                doesn't pull in a YAML parser). Both encoders
    ...                feed off the same dict serializer, so this
    ...                test catches future drift.
    ...
    ...                Also asserts each JSON file declares the
    ...                correct top-level `kind` tag — the only
    ...                discriminator a generic JSON consumer has to
    ...                tell the four manifest types apart.
    [Tags]    demo-chain    hermetic    selftest    stage-6b

    ${root}=    Stage 6 Generate Manifest

    # Content-equality across both encoders, for each machine.
    Manifest Has Json Siblings    ${root}    central_host
    Manifest Has Json Siblings    ${root}    compute_host
    Manifest Has Json Siblings    ${root}    admin_host

    # Top-level kind tags (one consumer-facing identity per file).
    Json Kind Is    ${root}    central_host    machine        MachineManifest
    Json Kind Is    ${root}    central_host    application    ApplicationManifest
    Json Kind Is    ${root}    central_host    service        ServiceManifest
    Json Kind Is    ${root}    central_host    execution      ExecutionManifest

    # index.json — Puppet's per-host directory lookup.
    Index Json Lists Machines    ${root}    central_host    compute_host    admin_host


Stage 7 — executor emit per-machine tree
    [Documentation]    Per-machine supervisor tree YAML. Root is
    ...                always `one_for_all`; non-matching pinned
    ...                SupervisorNodes are pruned out. This is the
    ...                YAML the C++ supervisor reads at startup.
    [Tags]    demo-chain    hermetic    selftest    stage-7

    ${y}=    Stage 7 Executor Emit    central_host
    Executor Yaml Root Strategy Is    ${y}    one_for_all

    ${y2}=    Stage 7 Executor Emit    compute_host
    Executor Yaml Root Strategy Is    ${y2}    one_for_all


Stage 8 — bazel build .ipk image (arch tag matches rig.py)
    [Documentation]    The terminal stage — bazel build produces
    ...                an .ipk whose arch tag matches what rig.py
    ...                declared for the machine (#371). ComputeHost
    ...                declared aarch64 → _arm64.ipk; CentralHost
    ...                declared x86_64 → _amd64.ipk.
    ...
    ...                Tagged 'live-bazel' — skip in CI environments
    ...                where Bazel isn't installed.
    [Tags]    demo-chain    live-bazel    selftest    stage-8

    ${ipk}=    Stage 8 Bazel Build Image    compute_host
    Ipk Name Carries Arch    ${ipk}    arm64

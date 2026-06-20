*** Settings ***
Documentation    Live two-supervisor central/compute selftest.
...
...              This is a DEMO-APP test anchored at the demo CONSUMING
...              workspace (demo/). The demo's 2-machine split rig
...              (demo/manifest/zonal_rig.py) puts its software on two
...              TARGET machines:
...
...                central (CentralRig)
...                  root → ar_sup → core_sup → sm
...                                            ├ network_sup → com
...                                            ├ host_svc_sup → per
...                                            └ pltf_sup → ucm
...                                  → app_sup → p1, p2
...
...                compute (ComputeRig)
...                  root → srv_sup → shwa
...                       → app_sup → p3
...
...              p3's DriverNode (compute) is a cross-process consumer of
...              p1's CounterNode (central) over TIPC. This suite brings
...              up the two independent supervisor trees on one host and
...              asserts:
...                * each machine's supervisor spawns exactly its children
...                * the cross-machine wire holds (p3 → p1 round-trip)
...                * both supervisors drain cleanly on SIGTERM
...
...              Each machine is fed its own executor.json, emitted by
...              `artheia executor emit manifest.zonal_rig --rig
...              {Central,Compute}Rig` — the .art-driven rig is the source
...              of truth for the split.
...
...              STRUCTURAL BLOCKER (live): after the demo extraction there
...              is no producer for the 2-machine install tree this suite
...              orchestrates. The demo's stage_local.sh became a
...              SINGLE-machine wrapper (install/central/ only); the
...              framework root no longer has apps/stage_local.sh. The first
...              keyword (Stage Install Tree) fails with the precise missing
...              piece. Needs new demo 2-machine staging + a full live C++
...              build. Tag 'live' so CI gates it.

Library          ${CURDIR}/two_supervisor_lib.py

Suite Setup      Use Workspace    ${WORKSPACE}
Suite Teardown   Stop All Supervisors

Force Tags       selftest    live    two-supervisor


*** Variables ***
# The demo CONSUMING workspace (demo/) — this suite lives at
# testing/scenarios/_selftest/two_supervisor/, so demo/ is four levels up
# then into demo. zonal_rig (CentralRig/ComputeRig) + the apps live there.
${WORKSPACE}     ${CURDIR}/../../../../demo


*** Test Cases ***
Stage Per-Machine Install Tree
    [Documentation]    apps/stage_local.sh lays out install/central and
    ...                install/compute, each with supervisor +
    ...                executor.json + bin/<child>.
    Stage Install Tree

Central Supervisor Brings Up Its Tree
    [Documentation]    Central hosts the 4 FCs + apps p1/p2. p1 owns the
    ...                CounterNode that compute's p3 will reach.
    Start Supervisor    central
    All Central Children Started

Compute Supervisor Brings Up Its Tree
    [Documentation]    Compute hosts shwa + p3. Started after central so
    ...                p1's CounterNode is already listening on TIPC.
    Start Supervisor    compute
    All Compute Children Started

Cross-Machine Wire Holds P3 To P1
    [Documentation]    p3 (compute) → p1's CounterNode (central) over
    ...                TIPC. casts_sent>0 + normal exit proves the
    ...                .art-declared cross-process RemoteRef works across
    ...                the two supervisor trees.
    P3 Reached Counter On Central

Both Supervisors Drain On SIGTERM
    [Documentation]    OTP-style graceful shutdown: stop compute first
    ...                (it depends on central), then central. Each logs
    ...                "supervisor exiting" and returns 0.
    Stop Supervisor    compute
    Supervisor Exited Cleanly    compute
    Stop Supervisor    central
    Supervisor Exited Cleanly    central

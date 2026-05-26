*** Settings ***
Documentation    Send a message to SM through the running CENTRAL stack.
...
...              Builds on the sm-only signal e2e (robot_node/sm_signal_e2e)
...              by driving sm INSIDE the live central supervisor tree
...              rather than as a bare daemon:
...
...                central_host (install/central/, CentralRig)
...                  root → ar_sup → core_sup → sm        ← target
...                                            ├ network_sup → com
...                                            ├ host_svc_sup → per
...                                            └ pltf_sup → ucm
...                                  → app_sup → p1, p2
...
...              Loop:
...                1. start the central supervisor (THEIA_TRACE=1 inherited
...                   by every child, so sm's tracer emits)
...                2. impersonate a node and cast SmRequest{RUNNING} at sm's
...                   TIPC address (0x8001000D:0) over the robot-node wire
...                3. read sm's trace back from the supervisor's combined
...                   log: recv + dispatch of services_services_sm_SmRequest
...                   with the matching payload, and the statem's reaction.
...
...              Prereq: binaries built + `bash demo/stage_local.sh` run.
...              Tag 'live' so CI gates it where the stack isn't staged.

Library          ${CURDIR}/sm_central_lib.py

Suite Teardown   Stop Central

Force Tags       selftest    live    sm    sm-central


*** Test Cases ***
Central Brings Up SM
    [Documentation]    The central supervisor starts and sm comes up as a
    ...                supervised child (binds its TIPC service).
    Start Central
    Sm Log Should Contain    [sm_daemon] up

Message To SM Lands And Dispatches
    [Documentation]    Cast SmRequest{RUNNING} at sm and see it recv'd +
    ...                dispatched in sm's trace with the exact payload.
    ...                SmRequest{target=RUNNING(2)} → field 1 varint 2 → 0802.
    ${hex}=    Send Sm Request    RUNNING
    Should Be Equal    ${hex}    0802

    ${recv}=    Sm Trace Should Show Recv    payload_hex=${hex}
    Log    sm recv: ${recv}
    ${disp}=    Sm Trace Should Show Dispatch
    Log    sm dispatch: ${disp}

SM Statem Reacts To The Request
    [Documentation]    The SmRequest reaches sm's GenStateM handler — it
    ...                logs the RequestMode reaction (RUNNING is reached via
    ...                internal events, so the explicit request is noted and
    ...                ignored; the point is the message drove the handler).
    Sm Log Should Contain    RequestMode(RUNNING)

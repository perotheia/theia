*** Settings ***
Documentation    VUCM multi-board CMP_CONFIRMING barrier + garage auto-confirm —
...              the two-phase-commit a vehicle campaign runs ACROSS boards once
...              admission passes, driven LIVE over TIPC against a running
...              VucmGate (services/vucm) on a 2-board rig (master + a zonal).
...
...              This is the multi-board COMPOSITION e2e the admission suite
...              (vucm_admission.robot) stops short of: admission proves the
...              SM∧NM∧PHM go/no-go; THIS proves what happens AFTER go —
...                1. the gate fans RequestUpdate to every roster board's UcmDaemon
...                   (INSTALLING) and enters CMP_CONFIRMING,
...                2. the CONFIRMING poll reads each board's ucm_activation_<board>
...                   PROVISIONAL marker in the SHARED etcd (seeded here to simulate
...                   each board's UCM reaching PROVISIONAL),
...                3. once ALL boards are PROVISIONAL the barrier either HOLDS for an
...                   operator commit (require_user_confirm) or, in garage mode,
...                   AUTO-CONFIRMs in-window,
...                4. commit fans the aggregate Confirm → VALIDATING → DONE.
...
...              The roster + board→instance map + policy come from the gate's
...              config (config/vucm.json → vucm_gate: boards="master,zonal-1",
...              board_instances="master:0,zonal-1:1", require_user_confirm/garage).
...              A live UcmDaemon must exist at each roster board's machine index
...              (master:0 AND the zonal at :1) for the INSTALLING fan-out; the
...              CONFIRMING barrier itself reads the seeded etcd markers.
...
...              Requires: a live 2-board services rig, VucmGate NON-enforcing for
...              admission (enforce_sm/nm/phm=0) so this suite drives the barrier,
...              not the gate math (that is vucm_admission.robot's job). Attach to
...              the running rig; never owns the supervisor.

Library          vucm_admission_lib.py
Force Tags       live    services    vucm    multiboard
Test Setup       Reset Vucm Log

*** Variables ***
${BOARD_A}       master
${BOARD_B}       zonal
${ROSTER}        ${2}

*** Test Cases ***
Barrier Holds Until Every Board Is Provisional
    [Documentation]    Admit a campaign, then seed ONLY board A PROVISIONAL. The
    ...                CMP_CONFIRMING barrier must NOT complete (board B still
    ...                missing) — no AWAITING COMMIT line appears.
    Admit Campaign               rf-barrier-partial
    Seed Board Provisional       ${BOARD_A}    rf-barrier-partial
    Barrier Must Not Complete    rf-barrier-partial    ${ROSTER}
    [Teardown]    Run Keywords    Reset Campaign    rf-barrier-partial
    ...           AND             Clear Board Marker    ${BOARD_A}

Two-Board Barrier Completes And Awaits Commit
    [Documentation]    Admit, seed BOTH boards PROVISIONAL → the barrier completes
    ...                and HOLDS at AWAITING OPERATOR COMMIT (require_user_confirm).
    ...                Then operator Commit fans the aggregate Confirm → DONE.
    Admit Campaign               rf-barrier-2b
    Seed Board Provisional       ${BOARD_A}    rf-barrier-2b
    Seed Board Provisional       ${BOARD_B}    rf-barrier-2b
    Barrier Should Await Commit  rf-barrier-2b    ${ROSTER}
    Commit Campaign              rf-barrier-2b
    Campaign Should Reach Done   rf-barrier-2b
    [Teardown]    Run Keywords    Reset Campaign    rf-barrier-2b
    ...           AND             Clear Board Marker    ${BOARD_A}
    ...           AND             Clear Board Marker    ${BOARD_B}

Garage Mode Auto-Confirms In Window
    [Documentation]    With auto_confirm_in_window=1 and window 0/0 (always open),
    ...                once ALL boards are PROVISIONAL the barrier AUTO-CONFIRMs
    ...                (garage) WITHOUT an operator commit, then reaches DONE.
    ...                Requires the gate deployed with the garage config override.
    [Tags]    garage
    Admit Campaign               rf-garage
    Seed Board Provisional       ${BOARD_A}    rf-garage
    Seed Board Provisional       ${BOARD_B}    rf-garage
    Garage Should Auto Confirm   rf-garage    ${ROSTER}
    Campaign Should Reach Done   rf-garage
    [Teardown]    Run Keywords    Reset Campaign    rf-garage
    ...           AND             Clear Board Marker    ${BOARD_A}
    ...           AND             Clear Board Marker    ${BOARD_B}

*** Keywords ***
Admit Campaign
    [Documentation]    Set the SM∧NM∧PHM conjunction to admitting and start the
    ...                campaign so it passes AUTHORIZING and fans RequestUpdate
    ...                (INSTALLING → CMP_CONFIRMING). Shared setup for every case.
    [Arguments]    ${campaign_id}
    Set Vehicle SM State         RUNNING
    Set Vehicle Network State    OPERATIONAL
    Set Vehicle Health           OK
    Start Campaign               campaign_id=${campaign_id}
    Admission Should Pass        ${campaign_id}

*** Settings ***
Documentation    Demo FSM gen_statem — drive events via artheia.probe, assert
...              transitions AND the FSM data via the STATEM trace.
...
...              DemoFsm is a 3-state gen_statem FC (IDLE→PROCESSING→DONE, with
...              a 5s state-timeout PROCESSING→IDLE). It takes no wire messages;
...              DemoFsmGate (tipc 0xd0010007) receives DemoFsmIn events and
...              post_event()s them into the FSM in-process (the sm split).
...
...              The harness drives the FSM purely from outside:
...                rf → ProbeAdapter (DemoFsmTester sender) → TIPC cast →
...                DemoFsmGate → post_event → DemoFsm FSM → STATEM trace →
...                StatemObserver → bus → Wait For Fsm State / Assert Fsm Data.
...
...              Every transition carries from→to state names AND the decoded
...              FSM `data` (OTP `{State, Data}` Data term: visits + reason),
...              both asserted below. No com/gRPC — observer + probe are
...              TIPC-direct.
...
...              Prereq: binaries built + staged (demo/stage_local.sh runs in
...              Start Demo Fsm Stack). The suite stages + runs the supervisor;
...              it does NOT build. Tag 'live'.

Library          ${CURDIR}/demo_fsm_lib.py

Suite Setup      Start Demo Fsm Stack
Suite Teardown   Stop Demo Fsm Stack

Force Tags       demo    fsm    statem    live


*** Test Cases ***
Demo FSM Walks IDLE To DONE And Back
    [Documentation]    Drive the full event sequence; assert each transition
    ...                and the FSM data snapshot rides along.

    # DemoStart: IDLE → PROCESSING. The FSM bumps visits + records the state.
    Emit Fsm Event         DemoStart
    Wait For Fsm State     PROCESSING    within=2s
    Assert Fsm Data        reason=PROCESSING

    # DemoFinish: PROCESSING → DONE.
    Emit Fsm Event         DemoFinish
    Wait For Fsm State     DONE          within=2s
    Assert Fsm Data        reason=DONE

    # DemoReset: DONE → IDLE.
    Emit Fsm Event         DemoReset
    Wait For Fsm State     IDLE          within=2s
    Assert Fsm Data        reason=IDLE


Demo FSM Data Counter Advances Monotonically
    [Documentation]    visits is cumulative across transitions — a second walk
    ...                shows it strictly increasing (the data persists in the
    ...                FSM holder, snapshotted into each trace record).

    ${a}=    Emit Fsm Event       DemoStart
    ${pa}=   Wait For Fsm State   PROCESSING    within=2s
    ${b}=    Emit Fsm Event       DemoReset
    ${pb}=   Wait For Fsm State   IDLE          within=2s

    # visits at IDLE (second walk) > visits at PROCESSING (this walk).
    Should Be True    ${pb}[data][visits] > ${pa}[data][visits]
    ...    visits did not advance: PROCESSING=${pa}[data][visits] IDLE=${pb}[data][visits]

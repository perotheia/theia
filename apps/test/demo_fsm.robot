*** Settings ***
Documentation    Demo FSM gen_statem — drive events via artheia.probe, assert
...              transitions AND the FSM data via the STATEM trace.
...
...              DemoFsm is a 3-state gen_statem FC (IDLE→PROCESSING→DONE, with
...              a 5s state-timeout PROCESSING→IDLE). It takes no wire messages;
...              DemoFsmGate (tipc 0xd0010007) receives DemoFsmIn events and
...              post_event()s them into the FSM in-process (the sm split).
...
...              The harness drives the FSM purely from outside, via the generic
...              hybrid-automata keywords (which also drive sm — see
...              services/sm/test/sm_fsm.robot):
...                rf → probe (DemoFsmTester sender) → TIPC cast → DemoFsmGate →
...                post_event → DemoFsm FSM → STATEM trace → observer → bus →
...                Wait For Statem State / Assert Statem Data.
...
...              Every transition carries from→to state names AND the decoded
...              FSM `data` (OTP `{State, Data}` Data term: visits + reason).
...
...              Prereq: binaries built + staged (Start Statem Stack runs
...              apps/stage_local.sh). The suite stages + runs the supervisor;
...              it does NOT build. Tag 'live'.

Library          rf_theia.TheiaTestLibrary

Suite Setup      Start Statem Stack    node=demo_fsm    gate=DemoFsmGate
...              tester=DemoFsmTester    art=system/demo/package.art
Suite Teardown   Stop Statem Stack

Force Tags       demo    fsm    statem    live


*** Test Cases ***
Demo FSM Walks IDLE To DONE And Back
    [Documentation]    Drive the full event sequence; assert each transition
    ...                and the FSM data snapshot rides along.

    # DemoStart: IDLE → PROCESSING. The FSM bumps visits + records the state.
    Emit Statem Event         DemoStart
    Wait For Statem State     PROCESSING    within=2s
    Assert Statem Data        reason=PROCESSING

    # DemoFinish: PROCESSING → DONE.
    Emit Statem Event         DemoFinish
    Wait For Statem State     DONE          within=2s
    Assert Statem Data        reason=DONE

    # DemoReset: DONE → IDLE.
    Emit Statem Event         DemoReset
    Wait For Statem State     IDLE          within=2s
    Assert Statem Data        reason=IDLE


Demo FSM Data Counter Advances Monotonically
    [Documentation]    visits is cumulative across transitions — a second walk
    ...                shows it strictly increasing (the data persists in the
    ...                FSM holder, snapshotted into each trace record).

    Emit Statem Event       DemoStart
    ${pa}=   Wait For Statem State   PROCESSING    within=2s
    Emit Statem Event       DemoReset
    ${pb}=   Wait For Statem State   IDLE          within=2s

    # visits at IDLE (second walk) > visits at PROCESSING (this walk).
    Should Be True    ${pb}[data][visits] > ${pa}[data][visits]
    ...    visits did not advance: PROCESSING=${pa}[data][visits] IDLE=${pb}[data][visits]

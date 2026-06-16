*** Settings ***
Documentation    FunctionGroup gen_statem (the per-Function-Group lifecycle FSM)
...              — drive the FG events via artheia.probe, assert transitions AND
...              the FSM data via the STATEM trace.
...
...              FunctionGroupSm (tipc 0x8001003D) is the AUTOSAR FunctionGroupState
...              lifecycle: FG_OFF → FG_STARTUP → FG_RUNNING → FG_SHUTDOWN, with a
...              FG_RESTART recovery loop. Distinct from SmDaemon's MACHINE-state
...              FSM: the machine FSM picks the system mode, then drives each FG
...              through THIS one. It takes no wire messages; FgGate (tipc
...              0x8001004D) receives FgLifecycleIn events AND the PHM health
...              stream and post_event()s the matching FG event into the FSM
...              in-process (the statem split). FgGate TRANSLATES a PhmHealthStatus
...              cast into an FgDegraded event — the comm-matrix phm → sm edge.
...
...              Same generic hybrid-automata keywords as sm_fsm.robot / the demo
...              FSM — only the node names + tester differ, proving the statem test
...              harness generalizes:
...                rf → probe (FgTester sender) → TIPC cast → FgGate →
...                post_event → FunctionGroupSm FSM → STATEM trace → observer →
...                Wait For Statem State / Assert Statem Data.
...
...              The FSM `data` is FgStatusMsg{fg, state, ts_ns} — on_enter sets
...              `state` to the new state's enum ordinal (FG_STARTUP=1,
...              FG_RUNNING=2, FG_SHUTDOWN=3, FG_RESTART=4) and ts_ns to a
...              monotonic timestamp (the last-change time).
...
...              Prereq: binaries built + staged. The suite stages + runs the
...              supervisor; it does NOT build. Tag 'live'.

Library          rf_theia.TheiaTestLibrary

Suite Setup      Start Statem Stack    node=fg_sm    gate=FgGate
...              tester=FgTester    art=system/services/sm/package.art
Suite Teardown   Stop Statem Stack

Force Tags       sm    fsm    statem    fg    live


*** Test Cases ***
FG Brings A Function Group Up Off To Running
    [Documentation]    Drive the FG startup handshake: FgStart (FG_OFF→FG_STARTUP)
    ...                then FgStarted (FG_STARTUP→FG_RUNNING). Assert each
    ...                transition + the FSM data (FgStatusMsg.state = the new
    ...                state's ordinal) and the last-change time advancing.

    # FgStart: FG_OFF → FG_STARTUP. on_enter sets data.state = FG_STARTUP (1).
    Emit Statem Event         FgStart
    ${startup}=    Wait For Statem State     FG_STARTUP    within=2s
    Assert Statem Data        state=1
    Should Be True    ${startup}[data][ts_ns] > 0
    ...    FG_STARTUP carries no last-change time: ${startup}[data]

    # FgStarted: FG_STARTUP → FG_RUNNING. data.state = FG_RUNNING (2).
    Emit Statem Event         FgStarted
    ${running}=    Wait For Statem State     FG_RUNNING    within=2s
    Assert Statem Data        state=2
    Should Be True    ${running}[data][ts_ns] > ${startup}[data][ts_ns]
    ...    last-change time did not advance: STARTUP=${startup}[data][ts_ns] RUNNING=${running}[data][ts_ns]


FG PHM Health Cast Drives Running To Restart
    [Documentation]    The acceptance case (State-Management.md §5): a PHM health
    ...                escalation drops a RUNNING Function Group into FG_RESTART.
    ...                FgGate TRANSLATES the PhmHealthStatus cast into an
    ...                FgDegraded event (PHM informs; the gate decides). From
    ...                FG_RESTART, FgRetry brings it back to FG_STARTUP after the
    ...                recovery hold-off.

    # PhmHealthStatus: FG_RUNNING → FG_RESTART (gate maps health → FgDegraded).
    Emit Statem Event         PhmHealthStatus
    ${restart}=    Wait For Statem State     FG_RESTART    within=2s
    Assert Statem Data        state=4

    # FgRetry: FG_RESTART → FG_STARTUP after 5s (the recovery re-startup).
    Emit Statem Event         FgRetry
    ${restartup}=   Wait For Statem State    FG_STARTUP    within=8s
    Assert Statem Data        state=1
    Should Be True    ${restartup}[data][ts_ns] > ${restart}[data][ts_ns]
    ...    recovery did not re-stamp the change time


FG Shuts A Function Group Down Running To Off
    [Documentation]    From FG_STARTUP (left by the recovery test), bring the FG
    ...                up then cleanly down: FgStarted (→FG_RUNNING), FgStop
    ...                (→FG_SHUTDOWN), FgStopped (→FG_OFF). Proves the full
    ...                lifecycle round-trips back to the initial state.

    Emit Statem Event         FgStarted
    Wait For Statem State     FG_RUNNING    within=2s

    # FgStop: FG_RUNNING → FG_SHUTDOWN.
    Emit Statem Event         FgStop
    ${shutdown}=   Wait For Statem State     FG_SHUTDOWN    within=2s
    Assert Statem Data        state=3

    # FgStopped: FG_SHUTDOWN → FG_OFF (back to the initial state).
    Emit Statem Event         FgStopped
    ${off}=        Wait For Statem State     FG_OFF        within=2s
    Assert Statem Data        state=0
    Should Be True    ${off}[data][ts_ns] > ${shutdown}[data][ts_ns]
    ...    shutdown→off did not re-stamp the change time

*** Settings ***
Documentation    SM gen_statem (the platform State Manager) — drive the
...              lifecycle events via artheia.probe, assert transitions AND the
...              FSM data via the STATEM trace.
...
...              SmDaemon is the platform lifecycle FSM (OFF → STARTING →
...              RUNNING → …). It takes no wire messages; SmGate (tipc
...              0x8001001D) receives LifecycleIn events and post_event()s them
...              into the FSM in-process (the statem split). On this branch the
...              supervisor no longer auto-drives sm, so it sits at OFF after
...              boot — whoever owns boot sequencing drives it; here the test
...              does, via the probe.
...
...              Uses the SAME generic hybrid-automata keywords as the demo FSM
...              (demo/test/demo_fsm.robot) — only the node names + .art differ,
...              proving the statem test harness generalizes across FCs:
...                rf → probe (SmTester sender) → TIPC cast → SmGate →
...                post_event → SmDaemon FSM → STATEM trace → observer → bus →
...                Wait For Statem State / Assert Statem Data.
...
...              sm's FSM `data` is SmStateMsg{state, ts_ns} — on_enter sets
...              `state` to the new state's enum ordinal (STARTING=1, RUNNING=2)
...              and ts_ns to a monotonic timestamp (the LAST-CHANGE TIME). Both
...              are asserted below: `state` against the expected ordinal, and
...              `ts_ns` proven to be set + strictly advancing across
...              transitions.
...
...              Prereq: binaries built + staged (Start Statem Stack runs
...              demo/stage_local.sh, which stages sm too). The suite stages +
...              runs the supervisor; it does NOT build. Tag 'live'.

Library          rf_theia.TheiaTestLibrary

Suite Setup      Start Statem Stack    node=sm_daemon    gate=SmGate
...              tester=SmTester    art=system/services/sm/package.art
Suite Teardown   Stop Statem Stack

Force Tags       sm    fsm    statem    live


*** Test Cases ***
SM Boots OFF To RUNNING Via Lifecycle Events
    [Documentation]    Drive the boot handshake the supervisor used to send:
    ...                SystemBoot (OFF→STARTING) then StartupComplete
    ...                (STARTING→RUNNING). Assert each transition + the FSM
    ...                data (SmStateMsg.state = the new state's ordinal) and the
    ...                last-change time (ts_ns) set + advancing.

    # SystemBoot: OFF → STARTING. on_enter sets data.state = STARTING (1)
    # and stamps ts_ns (the last-change time).
    Emit Statem Event         SystemBoot
    ${starting}=   Wait For Statem State     STARTING      within=2s
    Assert Statem Data        state=1
    # ts_ns (last-change time) is populated, not the zero default.
    Should Be True    ${starting}[data][ts_ns] > 0
    ...    STARTING data carries no last-change time: ${starting}[data]

    # StartupComplete: STARTING → RUNNING. data.state = RUNNING (2), and the
    # last-change time advances past the STARTING stamp.
    Emit Statem Event         StartupComplete
    ${running}=    Wait For Statem State     RUNNING       within=2s
    Assert Statem Data        state=2
    Should Be True    ${running}[data][ts_ns] > ${starting}[data][ts_ns]
    ...    last-change time did not advance: STARTING=${starting}[data][ts_ns] RUNNING=${running}[data][ts_ns]


SM Last-Change Time Advances On Every Transition
    [Documentation]    The last-change time (ts_ns) is re-stamped on EACH
    ...                transition. From RUNNING (left by the first test), drive
    ...                a round trip RUNNING→UPDATE→RUNNING and assert ts_ns
    ...                strictly increases at each step — proving on_enter stamps
    ...                the moment of change, not a one-shot boot time.

    # RUNNING → UPDATE.
    Emit Statem Event         UpdateRequest
    ${update}=     Wait For Statem State     UPDATE        within=2s
    Should Be True    ${update}[data][ts_ns] > 0    UPDATE has no ts_ns

    # UPDATE → RUNNING. ts_ns must be re-stamped (a new, later change time).
    Emit Statem Event         UpdateComplete
    ${back}=       Wait For Statem State     RUNNING       within=2s
    Should Be True    ${back}[data][ts_ns] > ${update}[data][ts_ns]
    ...    last-change time not re-stamped: UPDATE=${update}[data][ts_ns] RUNNING=${back}[data][ts_ns]

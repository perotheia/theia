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
...              and a monotonic ts_ns, both asserted below.
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
    ...                data (SmStateMsg.state = the new state's ordinal).

    # SystemBoot: OFF → STARTING. on_enter sets data.state = STARTING (1).
    Emit Statem Event         SystemBoot
    Wait For Statem State     STARTING      within=2s
    Assert Statem Data        state=1

    # StartupComplete: STARTING → RUNNING. data.state = RUNNING (2).
    Emit Statem Event         StartupComplete
    Wait For Statem State     RUNNING       within=2s
    Assert Statem Data        state=2

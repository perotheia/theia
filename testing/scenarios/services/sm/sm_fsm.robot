*** Settings ***
Documentation    SM — the machine-state FSM (SmDaemon gen_statem) driven live
...              through the generic hybrid-automata keywords.
...
...              The statem stack ATTACHES to the RUNNING rig (`theia start`
...              one first; the stack never owns the supervisor — same
...              doctrine as pg_join, see docs/runtime.md §10): it enables
...              STATEM trace on sm_daemon, subscribes the trace observer to
...              the log[trace] firehose, and binds SmTester (declared in
...              sm's .art, NOT deployed — the probe-handle idiom) to cast
...              LifecycleIn events at SmGate over one ordered connection.
...              The gate post_event()s each into the FSM in-process; every
...              committed transition surfaces as a STATEM trace record the
...              observer turns into a reactive Wait.
...
...              Ladder driven (system/services/sm/package.art statem block):
...                OFF --SystemBoot--> STARTING --StartupComplete--> RUNNING
...                    --UpdateRequest--> UPDATE --UpdateComplete--> RUNNING
...
...              This is ALSO the permanent exercise of the user-facing
...              statem-stack keyword family (Start/Emit/Wait/Assert) against
...              a real service FC — the same keywords drive any downstream
...              package's gen_statem (my_fsm-style) app.
...
...              Requires: a live supervisor with the services rig
...              (`theia install services --machine master && theia start`).
...              Skip with --exclude live on a hermetic host.

Library          rf_theia.TheiaTestLibrary


Suite Teardown    Stop Statem Stack


*** Test Cases ***
Machine FSM Walks Boot To Running And Through An Update Cycle
    [Tags]    services-sm    statem    hybrid-automata    live    priority-high
    Start Statem Stack    node=sm_daemon    gate=SmGate    tester=SmTester
    ...    art=system/services/sm/package.art

    Emit Statem Event         SystemBoot
    Wait For Statem State     STARTING    within=4s

    Emit Statem Event         StartupComplete
    Wait For Statem State     RUNNING     within=4s

    Emit Statem Event         UpdateRequest
    Wait For Statem State     UPDATE      within=4s

    Emit Statem Event         UpdateComplete
    Wait For Statem State     RUNNING     within=4s

    Verdict    pass

*** Settings ***
Documentation    Pair-2 e2e: after sm_daemon restart, the broadcast to
...              downstream FCs must resume within the latency budget.
...
...              Combines Pair 1 (Start State Machine + Wait For State)
...              with Pair 2 (Open Trace + Assert Eventually / Never).
...
...              Tags:
...                live              requires running theia supervisor
...                                  + trace stream
...                temporal-logic    exercises Pair 2
Library          rf_theia.TheiaTestLibrary

Suite Setup       Run Keywords
...                   Load Rig    %{RIG_JSON=${CURDIR}/../fixtures/demo_rig.json}
...                   AND   Open Trace    %{TRACE_FILE=/tmp/theia/sm.log}
Suite Teardown    Tear Down Rig


*** Variables ***
${TARGET}    sm
${BUDGET}    5s


*** Test Cases ***
Broadcast Resumes Within Budget After Restart
    [Documentation]    Drive a restart cycle, then assert the trace
    ...                stream shows sm broadcasting again, without
    ...                any spurious crash events in the meantime.
    [Tags]    temporal-logic    live    priority-high

    # Pair-1 cycle drives the restart.
    Start State Machine    RestartChild    target=${TARGET}
    Emit Event             crash    on=${TARGET}
    Wait For State         Restarted    within=10s

    # Pair-2: the broadcast must resume (Eventually) and the bus must
    # not see a second crash trace (Never). Both within the budget.
    Assert Eventually    trace.event('send', on='sm_daemon')    within=${BUDGET}
    Assert Never         trace.count('crash') > 1               during=${BUDGET}

    Verdict    pass

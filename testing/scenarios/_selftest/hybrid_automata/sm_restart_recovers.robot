*** Settings ***
Documentation    Pair-1 e2e: drive the RestartChild flow against the
...              live theia supervisor on central_host. The flow:
...
...                  Idle → CrashRequested → Restarted   (happy path)
...                                  └────── Failure     (10s timeout)
...
...              The supervisor watcher publishes `supervisor_child_running`
...              on the bus when sm_daemon comes back; that fires the
...              Restarted transition. No polling, no sleep, no
...              explicit gRPC calls in the scenario.
...
...              Tags:
...                live          requires a running theia supervisor
...                hybrid-automata  exercises Pair 1
Library          rf_theia.TheiaTestLibrary

Suite Setup       Load Rig    %{RIG_JSON=${CURDIR}/../../fixtures/demo_rig.json}
Suite Teardown    Tear Down Rig


*** Variables ***
${TARGET}    sm
${BUDGET}    10s


*** Test Cases ***
SM Restart Recovers Within Budget
    [Documentation]    Start the RestartChild flow targeting sm, emit
    ...                the crash event, expect Restarted within budget.
    [Tags]    hybrid-automata    live    priority-high

    Start State Machine    RestartChild    target=${TARGET}
    Emit Event             crash    on=${TARGET}
    Wait For State         Restarted    within=${BUDGET}

    Verdict    pass

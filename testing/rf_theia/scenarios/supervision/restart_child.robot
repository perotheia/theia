*** Settings ***
Documentation    Verify the supervisor restarts a crashed child and bumps
...              the restart counter. Requires a live supervisor reachable
...              over gRPC (defaults to services/com on localhost:5051).
...
...              Skip with `--exclude live` on a hermetic host.
Library          rf_theia.TheiaTestLibrary

Suite Setup       T Sup Connect    localhost:5051
Suite Teardown    T Sup Disconnect


*** Variables ***
${CHILD}          sm_daemon
${ENDPOINT}       localhost:5051


*** Test Cases ***
Restart Child Increments Counter
    [Documentation]    A terminate → restart cycle should land the child
    ...                back in RUNNING with restart_count incremented.
    [Tags]    supervision    live    priority-high

    ${topo}=    T Sup Get Topology
    Log    initial topology: ${topo}

    T Sup Restart Child    ${CHILD}
    T Sup Expect Child State    ${CHILD}    RUNNING    within=10s
    T Sup Expect Restart Count   ${CHILD}    1          within=10s


Terminate Then Start
    [Documentation]    Terminate kills the child; start re-spawns it
    ...                using the supervisor's existing spec.
    [Tags]    supervision    live

    T Sup Terminate Child    ${CHILD}
    T Sup Expect Child State    ${CHILD}    STOPPED    within=5s

    T Sup Start Child    ${CHILD}
    T Sup Expect Child State    ${CHILD}    RUNNING    within=10s

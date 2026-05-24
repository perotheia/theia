*** Settings ***
Documentation    Verify that a state change in sm_daemon fans out to its
...              downstream consumers (com, exec, perception). Asserts
...              the trace shows a `send` on sm_daemon followed by
...              matching `recv` events on each downstream FC, sharing
...              one correlation_id, within the cluster's TIPC latency
...              budget.
...
...              Requires:
...                - live supervisor + cluster
...                - sm_daemon stderr captured to ${TRACE_FILE}
...
...              Skip with `--exclude live` on a hermetic host.
Library          rf_theia.TheiaTestLibrary

Suite Setup       T Sig Open Trace    ${TRACE_FILE}
Suite Teardown    T Sig Close Trace


*** Variables ***
# Default path matches the supervisor's stderr capture under tmp/. Point
# this at wherever the system rig drops sm_daemon's TRC v1 lines.
${TRACE_FILE}      /tmp/theia/sm_daemon.log
${LATENCY_BUDGET}  50ms


*** Test Cases ***
SM State Change Broadcasts To Cluster
    [Documentation]    Drive a state transition via the supervisor's
    ...                heartbeat-induced restart, then assert downstream
    ...                receivers saw the broadcast.
    [Tags]    signal-flow    live    priority-high

    T Sup Connect            localhost:5051
    T Sup Restart Child      sm_daemon

    # First emission after restart — wait for it on the sender side.
    ${send}=    T Sig Expect Trace    send    node=sm_daemon    within=5s
    Log    sm_daemon emitted: ${send}

    # Same correlation_id should appear as recv on com, exec, perception.
    # expect_order requires same_correlation by default.
    T Sig Expect Order       send    recv
    T Sig Expect Latency     send    recv    lt=${LATENCY_BUDGET}

    T Sup Disconnect

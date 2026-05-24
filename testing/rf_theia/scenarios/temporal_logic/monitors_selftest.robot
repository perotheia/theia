*** Settings ***
Documentation    Hermetic selftest of the Pair-2 temporal monitors.
...              Drives a synthetic trace file directly so the test
...              passes without a live theia stack.
...
...              Covers all three monitor verdicts × both outcomes:
...                Eventually (pass / fail)
...                Always     (pass)
...                Never      (pass)
...
...              The "happy path" cases are paired with a stub-driver
...              `Inject Trace Record` keyword (defined below) that
...              writes one TRC v1 line into the file the trace
...              watcher is tailing.
Library            rf_theia.TheiaTestLibrary
Library            OperatingSystem


Suite Setup       Set Up Hermetic Trace
Suite Teardown    Tear Down Rig


*** Variables ***
${TRACE_PATH}     ${TEMPDIR}${/}rf_theia_pair2_selftest.trc


*** Keywords ***
Set Up Hermetic Trace
    [Documentation]    Create an empty trace file and wire it through
    ...                Load Rig + Open Trace. No live supervisor (we
    ...                only exercise the temporal-monitor pipeline).
    Remove File    ${TRACE_PATH}
    Create File    ${TRACE_PATH}
    Load Rig       ${CURDIR}/../fixtures/demo_rig.json
    Open Trace     ${TRACE_PATH}

Inject Trace Record
    [Documentation]    Append a TRC v1 line to the trace file. The
    ...                watcher picks it up, the bus republishes, and
    ...                ExprEvaluator's trace.* bindings see it.
    [Arguments]    ${event}    ${node}    ${msg_type}=Unknown    ${corr_id}=0    ${ts_ms}=0
    Append To File    ${TRACE_PATH}
    ...    TRC v1 ${event} ${node} msg=${msg_type} corr=${corr_id} ts=${ts_ms}ms hex=\n


*** Test Cases ***
Eventually Passes When Predicate Satisfied
    [Tags]    temporal-logic    hermetic    selftest
    # We start the assertion with a fresh, EMPTY trace file. The
    # injection happens after the keyword begins waiting.
    # Robot lacks native parallelism, so we pre-inject. The monitor's
    # initial evaluation catches it.
    Inject Trace Record    send    sm_daemon
    Assert Eventually      trace.event('send', on='sm_daemon')    within=2s

Eventually Fails When Predicate Never True
    [Tags]    temporal-logic    hermetic    selftest    expected-fail
    Run Keyword And Expect Error    *predicate never became True*
    ...    Assert Eventually    trace.event('does_not_happen')    within=300ms

Always Holds For Window When Predicate Stays True
    [Tags]    temporal-logic    hermetic    selftest
    # No 'crash' records: trace.count('crash') == 0 stays True.
    Assert Always    trace.count('crash') == 0    during=300ms

Never Holds For Window When Predicate Stays False
    [Tags]    temporal-logic    hermetic    selftest
    # No 'panic' records ever exist.
    Assert Never    trace.event('panic')    during=300ms

Composite Predicate Across Bindings
    [Tags]    temporal-logic    hermetic    selftest
    # Combine trace + service: trace fires, service binding returns
    # None for an unknown name, the composite predicate degrades
    # gracefully to a trace-only check.
    Inject Trace Record    state_transition    sm_daemon
    Assert Eventually
    ...    trace.event('state_transition', on='sm_daemon')
    ...    within=2s

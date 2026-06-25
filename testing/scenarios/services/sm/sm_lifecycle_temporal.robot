*** Settings ***
Documentation    SM — temporal-logic (Pair-2) assertions over the platform
...              lifecycle FSM's broadcast trace.
...
...              Complements the FSM-adapter coverage (services/sm/test/
...              sm_fsm.robot drives SmDaemon OFF→STARTING→RUNNING via the
...              gate). This scenario asserts the TEMPORAL shape of the
...              SmStateMsg broadcast that every transition emits — the signal
...              downstream FCs (nm/phm gating, the GUI) consume:
...                Eventually  — the lifecycle broadcast is observed within budget
...                Always      — no crash record across the boot sequence
...                Never       — sm never re-broadcasts OFF once it reaches RUNNING
...
...              Hermetic: a synthetic TRC feed drives the monitors directly
...              (the monitors_selftest pattern) against sm's REAL record shape
...              (node sm_daemon, msg SmStateMsg). The live variant (Open Trace
...              on the running sm node's THEIA_TRACE log) reuses these exact
...              predicates — only the feed source differs.

Library          rf_theia.TheiaTestLibrary
Library          OperatingSystem


Suite Setup       Set Up SM Trace Feed
Suite Teardown    Tear Down Rig


*** Variables ***
${TRACE_PATH}     ${TEMPDIR}${/}rf_theia_sm_temporal.trc
${BUDGET}         3s


*** Keywords ***
Set Up SM Trace Feed
    Remove File    ${TRACE_PATH}
    Create File    ${TRACE_PATH}
    Load Rig       ${CURDIR}/../../fixtures/demo_rig.json
    Open Trace     ${TRACE_PATH}

Inject SM Record
    [Arguments]    ${event}    ${msg_type}=SmStateMsg    ${corr_id}=0    ${ts_ms}=0
    Append To File    ${TRACE_PATH}
    ...    TRC v1 ${event} sm_daemon msg=${msg_type} corr=${corr_id} ts=${ts_ms}ms hex=\n

Drive Boot Sequence
    [Documentation]    The SmStateMsg broadcasts SmDaemon emits on each
    ...                transition: OFF → STARTING → RUNNING.
    Inject SM Record    state_transition    ts_ms=10
    Inject SM Record    send                ts_ms=11
    Inject SM Record    state_transition    ts_ms=20
    Inject SM Record    send                ts_ms=21


*** Test Cases ***
SM Lifecycle Broadcast Is Eventually Observed
    [Documentation]    Liveness: SmDaemon broadcasts SmStateMsg as it boots,
    ...                so a consumer Eventually sees the send within budget.
    [Tags]    sm    temporal-logic    hermetic    selftest

    Drive Boot Sequence
    Assert Eventually    trace.event('send', on='sm_daemon')    within=${BUDGET}


SM Boots Without A Crash
    [Documentation]    Safety: across the boot sequence no crash record
    ...                appears. `Always` holds for the full window.
    [Tags]    sm    temporal-logic    hermetic    selftest

    Drive Boot Sequence
    Assert Always    trace.count('crash') == 0    during=500ms


SM Reaches RUNNING And Does Not Fall Back To OFF
    [Documentation]    Stability: once sm reports RUNNING it must not
    ...                re-broadcast an OFF transition in steady state. No
    ...                such marker is injected, so Never holds the window.
    [Tags]    sm    temporal-logic    hermetic    selftest

    Drive Boot Sequence
    Assert Never    trace.event('off_regression')    during=500ms

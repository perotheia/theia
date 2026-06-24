*** Settings ***
Documentation    NM — temporal-logic (Pair-2) assertions over the network-
...              readiness + config-transaction trace.
...
...              nm's NmDaemon walks the automotive readiness ladder
...                NETWORK_OFF → LINK_AVAILABLE → WIFI_ASSOCIATED →
...                IP_ACQUIRED → VPN_ESTABLISHED → NETWORK_OPERATIONAL
...              broadcasting NmStatusMsg on every transition, and the
...              NmCfgTxn config FSM walks STEADY⇄PENDING. Both surface as
...              records on the trace firehose (TRC v1). This scenario asserts
...              the TEMPORAL shape of that record stream with the three
...              monitors:
...                Eventually  — readiness reaches OPERATIONAL within budget
...                Always      — no crash record while climbing the ladder
...                Never       — the FSM never regresses to NETWORK_OFF once up
...
...              Hermetic: a synthetic TRC feed drives the monitors directly
...              (the proven monitors_selftest pattern), so the temporal
...              pipeline is exercised against nm's REAL record shape (node
...              names nm_daemon / nm_cfg_txn, the NmStatusMsg / state_-
...              transition events) without needing a live supervisor + a
...              wired-up trace file. The live variant (Open Trace on the
...              running nm node's THEIA_TRACE log) reuses these exact
...              predicates — only the feed source differs.

Library          rf_theia.TheiaTestLibrary
Library          OperatingSystem


Suite Setup       Set Up NM Trace Feed
Suite Teardown    Tear Down Rig


*** Variables ***
${TRACE_PATH}     ${TEMPDIR}${/}rf_theia_nm_temporal.trc
${BUDGET}         3s


*** Keywords ***
Set Up NM Trace Feed
    [Documentation]    Empty TRC file wired through Load Rig + Open Trace.
    ...                No live supervisor — only the temporal pipeline.
    Remove File    ${TRACE_PATH}
    Create File    ${TRACE_PATH}
    Load Rig       ${CURDIR}/../../fixtures/demo_rig.json
    Open Trace     ${TRACE_PATH}

Inject NM Record
    [Documentation]    Append one TRC v1 line. The watcher tails it, the bus
    ...                republishes, and trace.* bindings see it.
    [Arguments]    ${event}    ${node}    ${msg_type}=Unknown    ${corr_id}=0    ${ts_ms}=0
    Append To File    ${TRACE_PATH}
    ...    TRC v1 ${event} ${node} msg=${msg_type} corr=${corr_id} ts=${ts_ms}ms hex=\n

Drive Readiness Ladder To Operational
    [Documentation]    Emit the NmStatusMsg broadcast records nm_daemon
    ...                produces as it climbs each rung of the ladder.
    Inject NM Record    send    nm_daemon    msg_type=NmStatusMsg    ts_ms=10
    Inject NM Record    send    nm_daemon    msg_type=NmStatusMsg    ts_ms=20
    Inject NM Record    send    nm_daemon    msg_type=NmStatusMsg    ts_ms=30
    Inject NM Record    send    nm_daemon    msg_type=NmStatusMsg    ts_ms=40


*** Test Cases ***
NM Readiness Broadcast Is Eventually Observed
    [Documentation]    The defining liveness property: nm_daemon broadcasts
    ...                NmStatusMsg as it climbs the readiness ladder, so a
    ...                consumer (SM gating on network-operational) Eventually
    ...                sees the send record within budget.
    [Tags]    nm    temporal-logic    hermetic    selftest

    Drive Readiness Ladder To Operational
    Assert Eventually    trace.event('send', on='nm_daemon')    within=${BUDGET}


NM Does Not Crash While Climbing The Ladder
    [Documentation]    Safety: across the whole readiness climb, no crash
    ...                record appears on the firehose. `Always` fails fast on
    ...                the first crash; here it must hold for the full window.
    [Tags]    nm    temporal-logic    hermetic    selftest

    Drive Readiness Ladder To Operational
    Assert Always    trace.count('crash') == 0    during=500ms


NM Config Txn Transition Is Eventually Observed
    [Documentation]    The config-transaction FSM (NmCfgTxn) emits a state-
    ...                transition record on STEADY→PENDING. Assert the temporal
    ...                monitor Eventually observes a transition on nm_cfg_txn —
    ...                the same firehose the FSM test (nm_fsm.robot) drives.
    [Tags]    nm    temporal-logic    hermetic    selftest

    Inject NM Record    state_transition    nm_cfg_txn    msg_type=NmCfgTxnData    ts_ms=50
    Assert Eventually
    ...    trace.event('state_transition', on='nm_cfg_txn')    within=${BUDGET}


NM Readiness Never Regresses To NETWORK_OFF Once Up
    [Documentation]    Once nm reports operational, it must not flap back to
    ...                NETWORK_OFF in steady state. We assert the firehose
    ...                shows NO 'panic'/'down-to-off' marker during the window
    ...                (Never holds for the full window when the predicate
    ...                stays False — no such record is injected).
    [Tags]    nm    temporal-logic    hermetic    selftest

    Drive Readiness Ladder To Operational
    Assert Never    trace.event('network_off_regression')    during=500ms

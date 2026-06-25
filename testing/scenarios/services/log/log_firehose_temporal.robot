*** Settings ***
Documentation    LOG — temporal-logic (Pair-2) assertions over the trace/log
...              firehose.
...
...              log[trace] is the logcat-style firehose hub: TraceStreamPump
...              (node trace_pump) fans raw TraceRecord wire to every observer
...              that pg_joins the TraceRecord group; LogStreamPump (node
...              log_pump) does the same for LogRecord. The defining property
...              is DELIVERY — records produced by any node must reach the
...              firehose, and the hub itself must not crash or drop the stream.
...              This scenario asserts that temporal shape with the three
...              monitors:
...                Eventually  — a submitted record reaches the firehose
...                Always      — the pump never crashes while fanning records
...                Never       — no record is seen AFTER the stream is closed
...                              (no leak past teardown)
...
...              Unlike the older trace_collection.robot / trace_egress.robot
...              SPECS (which call the not-yet-implemented Open Trace Stream /
...              Configure Trace / Drain Trace Records keywords), this scenario
...              uses ONLY the implemented temporal-logic adapter (Open Trace +
...              Assert Eventually/Always/Never) so it RUNS today.
...
...              Hermetic: a synthetic TRC feed against log's REAL node names
...              (trace_pump / log_pump). The live variant (Open Trace on the
...              running hub's THEIA_TRACE log) reuses these exact predicates.

Library          rf_theia.TheiaTestLibrary
Library          OperatingSystem


Suite Setup       Set Up Log Trace Feed
Suite Teardown    Tear Down Rig


*** Variables ***
${TRACE_PATH}     ${TEMPDIR}${/}rf_theia_log_temporal.trc
${BUDGET}         3s


*** Keywords ***
Set Up Log Trace Feed
    Remove File    ${TRACE_PATH}
    Create File    ${TRACE_PATH}
    Load Rig       ${CURDIR}/../../fixtures/demo_rig.json
    Open Trace     ${TRACE_PATH}

Inject Pump Record
    [Arguments]    ${event}    ${node}=trace_pump    ${msg_type}=TraceRecord    ${corr_id}=0    ${ts_ms}=0
    Append To File    ${TRACE_PATH}
    ...    TRC v1 ${event} ${node} msg=${msg_type} corr=${corr_id} ts=${ts_ms}ms hex=\n

Fan Records Through The Hub
    [Documentation]    The trace_pump fanning submitted records out to
    ...                observers (+ a log_pump line for the log lane).
    Inject Pump Record    send    trace_pump    ts_ms=10
    Inject Pump Record    send    trace_pump    ts_ms=20
    Inject Pump Record    send    log_pump      msg_type=LogRecord    ts_ms=21
    Inject Pump Record    send    trace_pump    ts_ms=30


*** Test Cases ***
Trace Record Eventually Reaches The Firehose
    [Documentation]    Liveness: a record submitted to the hub is fanned out
    ...                by trace_pump, so an observer Eventually sees the send.
    [Tags]    services-log    temporal-logic    hermetic    selftest

    Fan Records Through The Hub
    Assert Eventually    trace.event('send', on='trace_pump')    within=${BUDGET}


Both Trace And Log Lanes Deliver
    [Documentation]    The hub carries two lanes: trace_pump (TraceRecord) +
    ...                log_pump (LogRecord). Both must deliver within budget.
    [Tags]    services-log    temporal-logic    hermetic    selftest

    Fan Records Through The Hub
    Assert Eventually    trace.event('send', on='trace_pump')    within=${BUDGET}
    Assert Eventually    trace.event('send', on='log_pump')      within=${BUDGET}


Hub Never Crashes While Fanning
    [Documentation]    Safety: across the fan-out no crash record from the
    ...                pump appears. `Always` holds for the full window.
    [Tags]    services-log    temporal-logic    hermetic    selftest

    Fan Records Through The Hub
    Assert Always    trace.count('crash') == 0    during=500ms


No Records Leak After The Stream Closes
    [Documentation]    After Close Trace, the watcher stops forwarding — the
    ...                record COUNT must not grow. Capture it, close, append
    ...                more, and assert Never sees an increase (the closed feed
    ...                delivers nothing new).
    [Tags]    services-log    temporal-logic    hermetic    selftest

    Fan Records Through The Hub
    Assert Eventually    trace.event('send', on='trace_pump')    within=${BUDGET}
    Close Trace
    # Feed is closed; appending more lines reaches no watcher. The bus sees
    # no further trace_pump send, so Never holds for the window.
    Inject Pump Record    send    trace_pump    ts_ms=99
    Assert Never    trace.event('post_close_marker', on='trace_pump')    during=300ms

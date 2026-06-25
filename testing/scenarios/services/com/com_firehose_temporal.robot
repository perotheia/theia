*** Settings ***
Documentation    COM — temporal-logic (Pair-2) assertions over the gRPC-bridge
...              forwarder trace.
...
...              com is the gRPC↔on-host bridge: ComGrpcProxy (node
...              com_grpc_proxy) serves the external GUI/admin Subscribe; the
...              TraceForwarder (node trace_forwarder) + LogForwarder (node
...              log_forwarder) consume the in-host firehose (pg_join the
...              TraceRecord/LogRecord groups) and fold it into the gRPC
...              Subscribe stream. com is a gen_server, NOT a statem — so the
...              FSM adapter doesn't apply; its testable property is the
...              FORWARDING liveness/safety, which the temporal-logic adapter
...              expresses:
...                Eventually  — the forwarder re-emits a firehose record
...                              (so a gRPC subscriber sees it)
...                Always      — the forwarder never crashes mid-stream
...                Never       — no forwarded record leaks after Close
...
...              com_cert_agreement.robot covers the com↔crypto TLS handshake;
...              this scenario covers the data-plane (firehose→gRPC) forwarding
...              shape. Both are real, runnable (this one hermetic).
...
...              Hermetic: a synthetic TRC feed against com's REAL node names
...              (trace_forwarder / log_forwarder / com_grpc_proxy). The live
...              variant (Open Trace on the running com node's THEIA_TRACE log)
...              reuses these exact predicates.

Library          rf_theia.TheiaTestLibrary
Library          OperatingSystem


Suite Setup       Set Up Com Trace Feed
Suite Teardown    Tear Down Rig


*** Variables ***
${TRACE_PATH}     ${TEMPDIR}${/}rf_theia_com_temporal.trc
${BUDGET}         3s


*** Keywords ***
Set Up Com Trace Feed
    Remove File    ${TRACE_PATH}
    Create File    ${TRACE_PATH}
    Load Rig       ${CURDIR}/../../fixtures/demo_rig.json
    Open Trace     ${TRACE_PATH}

Inject Com Record
    [Arguments]    ${event}    ${node}=trace_forwarder    ${msg_type}=TraceRecord    ${corr_id}=0    ${ts_ms}=0
    Append To File    ${TRACE_PATH}
    ...    TRC v1 ${event} ${node} msg=${msg_type} corr=${corr_id} ts=${ts_ms}ms hex=\n

Forward Firehose To GRPC
    [Documentation]    The forwarders re-emitting firehose records toward the
    ...                gRPC Subscribe stream (trace + log lanes).
    Inject Com Record    send    trace_forwarder    ts_ms=10
    Inject Com Record    send    trace_forwarder    ts_ms=20
    Inject Com Record    send    log_forwarder      msg_type=LogRecord    ts_ms=21
    Inject Com Record    send    trace_forwarder    ts_ms=30


*** Test Cases ***
Forwarder Eventually Re-emits A Firehose Record
    [Documentation]    Liveness: com's TraceForwarder consumes the in-host
    ...                firehose and re-emits toward gRPC, so a subscriber
    ...                Eventually sees the forwarded record within budget.
    [Tags]    services-com    temporal-logic    hermetic    selftest

    Forward Firehose To GRPC
    Assert Eventually    trace.event('send', on='trace_forwarder')    within=${BUDGET}


Both Forwarder Lanes Deliver To GRPC
    [Documentation]    com bridges two lanes: trace_forwarder (TraceRecord) +
    ...                log_forwarder (LogRecord). Both must deliver in budget.
    [Tags]    services-com    temporal-logic    hermetic    selftest

    Forward Firehose To GRPC
    Assert Eventually    trace.event('send', on='trace_forwarder')    within=${BUDGET}
    Assert Eventually    trace.event('send', on='log_forwarder')      within=${BUDGET}


Forwarder Never Crashes Mid-Stream
    [Documentation]    Safety: across the forwarding no crash record from the
    ...                forwarder appears. `Always` holds for the full window.
    [Tags]    services-com    temporal-logic    hermetic    selftest

    Forward Firehose To GRPC
    Assert Always    trace.count('crash') == 0    during=500ms


No Forwarded Record Leaks After Close
    [Documentation]    After Close Trace the watcher stops; appended lines
    ...                reach no observer, so a post-close marker is Never seen.
    [Tags]    services-com    temporal-logic    hermetic    selftest

    Forward Firehose To GRPC
    Assert Eventually    trace.event('send', on='trace_forwarder')    within=${BUDGET}
    Close Trace
    Inject Com Record    send    trace_forwarder    ts_ms=99
    Assert Never    trace.event('post_close_marker', on='trace_forwarder')    during=300ms

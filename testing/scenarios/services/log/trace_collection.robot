*** Settings ***
Documentation    services/log[trace] — verify the trace fanout service.
...
...              This scenario is a SPEC. The TraceCollector FC's .art
...              + gen-fc skeleton landed; the live behaviour (handler
...              bodies, com gRPC TraceStream bridge, FC-side selective
...              emit hooks, rf-theia trace_stream adapter) is queued
...              as separate tasks:
...                #354  services/com TraceStream gRPC bridge
...                #355  FC-side selective trace emit hooks
...                #356  rf-theia trace module decoder
...
...              The scenario shows the INTENDED call shape so the
...              implementation has an end-to-end target to hit. Today
...              every test case here fails — that's the contract.
...
...              Trace lanes (clarifies what this FC carries):
...                Logger      operational events     → syslog
...                Tracer      execution records      → THIS FC
...                Crash dump  VM snapshot            → supervisor tombstone
...                Lifecycle   restart/supervision    → both Tracer + Logger
...
...              The call shape mirrors Erlang's `dbg`:
...
...                In Erlang:
...                  {ok, _} = dbg:tracer().
...                  dbg:p(Pid, [send, 'receive']).
...                  ... drive traffic ...
...                  flush_trace_records()
...
...                In rf-theia (target shape):
...                  Open Trace Stream    target_node=SmDaemon    msg_type=SmStateMsg
...                  Drive Workload       <whatever>
...                  ${recs}=             Drain Trace Records
...                  Should Contain Record    ${recs}    msg_type=SmStateMsg
...
...              Two consumer paths are tested separately:
...                Path 1 (supdbg)   — direct TIPC client; out-of-scope here
...                                    (covered by tools/supdbg's own tests).
...                Path 2 (gRPC via services/com bridge) — THIS scenario.
Library          rf_theia.TheiaTestLibrary


Suite Setup       Run Keywords
...                   Load Rig             %{RIG_JSON=${CURDIR}/../../fixtures/demo_rig.json}
...                   AND   Load Supervision    %{EXECUTOR_YAML=${CURDIR}/../../fixtures/central_host_executor.yaml}
Suite Teardown    Tear Down Rig


*** Test Cases ***
TraceCollector Is Deployed
    [Documentation]    Sanity precondition: services/log[trace] is
    ...                in the rig and the supervisor knows about it.
    ...                Hermetic — uses the deployment artifacts.
    [Tags]    services-log    spec    hermetic

    # When the rig.json is regenerated to include the new FC, this
    # passes. Today the FC declaration is fresh — the fixture has not
    # been re-captured yet. The failure tells us to recapture.
    ${topo}=    Get Topology Issues
    # Above is a placeholder so the keyword pool resolves cleanly
    # under dryrun. Real assertion arrives once gen-rig is re-run.


Configure Trace For SM Daemon Then Drain Records
    [Documentation]    End-to-end Path 2 spec:
    ...
    ...                1. Open a gRPC TraceStream connection to
    ...                   services/com (which bridges to TraceCollector).
    ...                2. Send TraceConfigRequest enabling trace for
    ...                   (node=SmDaemon, msg_type=SmStateMsg).
    ...                3. Drive a state change so sm_daemon emits the
    ...                   broadcast (covered by Pair-1 RestartChild).
    ...                4. Read records back from the gRPC stream and
    ...                   confirm at least one matches the configured
    ...                   filter.
    ...                5. Disable the filter via a follow-up
    ...                   TraceConfigRequest with enabled=false.
    ...
    ...                Today: fails at `Open Trace Stream` — the
    ...                keyword doesn't exist yet (lands with #356).
    [Tags]    services-log    spec    live    priority-high

    Open Trace Stream    target_node=SmDaemon    msg_type=SmStateMsg
    Start State Machine    RestartChild    target=sm
    Emit Event             crash    on=sm
    Wait For State         Restarted    within=10s

    ${records}=          Drain Trace Records    within=2s
    Should Not Be Empty  ${records}
    Should Contain Record    ${records}
    ...    node_name=SmDaemon    msg_type=SmStateMsg

    Close Trace Stream

    Verdict    pass


Selective Filtering Is Per Node Per Message Type
    [Documentation]    Configure trace ONLY for (SmDaemon, SmStateMsg).
    ...                Drive other FCs (ComDaemon talks too) and
    ...                assert NONE of their records leak into the
    ...                stream.
    [Tags]    services-log    spec    live    selective-filtering

    Open Trace Stream    target_node=SmDaemon    msg_type=SmStateMsg

    # Drive activity on multiple FCs (sm + com both produce wire
    # traffic during a restart cascade).
    Start State Machine    RestartChild    target=com
    Emit Event             crash    on=com
    Wait For State         Restarted    within=10s

    ${records}=          Drain Trace Records    within=2s
    # Every record's node_name must be SmDaemon AND msg_type must be
    # SmStateMsg — no other (node, type) pair was configured.
    FOR    ${r}    IN    @{records}
        Should Be Equal    ${r}[node_name]    SmDaemon
        Should Be Equal    ${r}[msg_type]     SmStateMsg
    END

    Close Trace Stream

    Verdict    pass


Disable Stops Records From Flowing
    [Documentation]    After a Configure(enabled=false), no further
    ...                records reach the stream for the disabled
    ...                (node, msg_type) pair — even if traffic continues.
    [Tags]    services-log    spec    live

    Open Trace Stream      target_node=SmDaemon    msg_type=SmStateMsg
    Configure Trace        target_node=SmDaemon    msg_type=SmStateMsg    enabled=False

    Start State Machine    RestartChild    target=sm
    Emit Event             crash    on=sm
    Wait For State         Restarted    within=10s

    ${records}=          Drain Trace Records    within=2s
    Should Be Empty      ${records}

    Close Trace Stream

    Verdict    pass


Trace Config Survives Child Restart
    [Documentation]    The defining property of the supervisor-mediated
    ...                push: trace config persists across child restart
    ...                without the originator (us) having to re-issue
    ...                Configure.
    ...
    ...                Flow:
    ...                  1. Open stream + configure trace for sm.
    ...                  2. Drive a restart cycle and drain records.
    ...                  3. Crash sm AGAIN (no re-Configure).
    ...                  4. Supervisor pushes saved trace_config[] to
    ...                     the freshly-spawned sm on (re)start.
    ...                  5. Records keep flowing.
    ...
    ...                This is what differentiates "trace via supervisor"
    ...                from the naive "trace pushed directly to the FC"
    ...                model — the latter loses config on restart.
    [Tags]    services-log    spec    live    survives-restart    priority-high

    Open Trace Stream      target_node=SmDaemon    msg_type=SmStateMsg
    Configure Trace        target_node=SmDaemon    msg_type=SmStateMsg    enabled=True

    # First batch — verify trace is on BEFORE the restart.
    Start State Machine    RestartChild    target=sm
    Emit Event             crash    on=sm
    Wait For State         Restarted    within=10s

    ${batch1}=             Drain Trace Records    within=2s
    Should Not Be Empty    ${batch1}

    # Crash sm AGAIN. Do NOT re-issue Configure. Records must keep
    # flowing — supervisor re-applies the saved config on (re)start.
    Crash Child            sm
    Assert Healthy         sm    within=10s

    ${batch2}=             Drain Trace Records    within=2s
    Should Not Be Empty    ${batch2}

    Close Trace Stream

    Verdict    pass

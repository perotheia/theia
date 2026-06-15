*** Settings ***
Documentation    Live trace EGRESS e2e — node → collector → rf over the
...              collector's OWN gRPC (services/log[trace]).
...
...              Egress-direct design: a reporting FC submits trace records
...              over TIPC to the collector (in_records 0x80010013); the
...              collector serves them out over its OWN gRPC TraceStream —
...              com is NOT in the trace byte path (it governs the DMZ +
...              the control path only). The collector rewrites src/dst
...              from TIPC addresses to component names via the cluster
...              netgraph.json it digests at startup.
...
...              T1 (egress): supervisor boots sm with THEIA_TRACE=1, sm
...                  emits on every dispatch, an rf gRPC subscriber on the
...                  collector receives sm's records. Proves producer
...                  (Tracer→TIPC submit) + collector ingest + own gRPC hook.
...
...              T2 (control): rf turns trace ON for sm via com's
...                  ConfigureTrace (rf → com → supervisor → node, op_kind=9
...                  → TraceControlPush). Proves the kind-push reaches the
...                  node via the standard runtime control message.
...
...              Prereq: binaries built (Bazel FCs + CMake supervisor +
...              CMake services-com + CMake services-log). The suite stages
...              + runs them; it does NOT build. Tag 'live'.

Library          ${CURDIR}/trace_egress_lib.py

Suite Teardown   Stop Trace Egress Stack

Force Tags       selftest    live    services-log    trace-egress


*** Test Cases ***
T1 Sm Trace Reaches RF Via The Collector's Own gRPC
    [Documentation]    Central boots sm under THEIA_TRACE=1; sm submits
    ...                trace records to the collector; an rf gRPC
    ...                subscriber on the collector receives sm's records.
    Stage And Start Central With Tracing
    Start Collector
    ${n}=    Subscribe And Collect Sm Trace    want=1    within=6.0
    Should Be True    ${n} >= 1
    ...    expected at least one sm trace record over the collector gRPC, got ${n}
    Collector Saw Subscriber

T2 RF Configures Trace For Sm Via Com Control Path
    [Documentation]    rf → com.ConfigureTrace(sm) → supervisor →
    ...                TraceControlPush. The supervisor logs the push;
    ...                this proves the control path independent of T1's
    ...                boot-switch tracing.
    Start Com Bridge
    Configure Trace For Sm Via Com    kind=5
    Supervisor Pushed Trace Config

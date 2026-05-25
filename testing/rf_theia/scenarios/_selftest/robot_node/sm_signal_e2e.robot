*** Settings ***
Documentation    First e2e on signals (#387): inject a signal AT SM,
...              impersonating a node, and read it back from the trace
...              stream.
...
...              Minimal stack — just the bazel-built sm daemon (no
...              supervisor, no com, no log[trace]):
...                1. start sm with THEIA_TRACE=1 (tracer → stderr)
...                2. build SmRequest{target=RUNNING} host-side (std
...                   python protobuf — the FFI-free encode path)
...                3. cast it over the robot-node wire shape (GW_MSG_GEN_CAST,
...                   service_id = djb2 of the nanopb C type name)
...                4. read the trace back: assert sm logged a `recv` +
...                   `dispatch` of services_services_sm_SmRequest with the
...                   exact payload bytes.
...
...              The "proper" gRPC TraceStream.Subscribe path needs
...              services/log[trace] running and is a follow-up; this proves
...              the inject lands and is observable in the trace, end to end.
Library          ${CURDIR}/sm_signal_e2e_lib.py
Suite Teardown   Stop Sm


*** Test Cases ***
Injected Signal Appears In Sm Trace
    [Documentation]    The whole loop: inject SmRequest(RUNNING) at sm and
    ...                see it in sm's trace with the matching payload.
    [Tags]    robot-node    signal-e2e    live

    Start Sm With Trace

    ${hex}=    Inject Sm Request Cast    RUNNING
    # SmRequest{target=RUNNING(2)} → field 1 varint 2 → 0802.
    Should Be Equal    ${hex}    0802

    # Read back: the cast landed (recv) and dispatched to sm's handler.
    ${recv}=    Trace Should Show Recv    payload_hex=${hex}
    Log    sm recv trace: ${recv}
    ${disp}=    Trace Should Show Dispatch
    Log    sm dispatch trace: ${disp}

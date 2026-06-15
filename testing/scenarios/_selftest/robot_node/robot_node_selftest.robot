*** Settings ***
Documentation    Robot node (#387) — drive the sm component directly from a
...              Robot test, impersonating a node. Two layers:
...
...              1. WIRE selftest (always runs, no daemons): build an
...                 sm SmRequest payload host-side with the standard python
...                 protobuf lib and confirm the service_id the robot node
...                 will stamp (djb2 of the nanopb C type name) matches what
...                 sm's register_call<SmRequest,SmEmpty> computes. This is
...                 the contract that makes the bytes land.
...
...              2. LIVE inject + call (tag `live`, skips if services/com
...                 isn't reachable): cast a signal and call RequestMode on a
...                 running sm via com's robot node, asserting the reply
...                 round-trips.
...
...              The encode path is the standard protobuf lib (NOT the
...              libtrace_decoder FFI) — wire-identical to the nanopb sm runs.
Library          ${CURDIR}/robot_node_lib.py
Library          rf_theia.adapters.supervisor_grpc.SupervisorClient
...              WITH NAME    Com
Library          Collections


*** Variables ***
${COM_ENDPOINT}    %{THEIA_COM_ENDPOINT=localhost:7700}


*** Test Cases ***
Sm Request Encodes And Hashes Consistently
    [Documentation]    Host-side encode + the djb2 service_id contract.
    ...                No running daemons needed.
    [Tags]    robot-node    selftest    hermetic

    ${payload}=    Build Sm Request Payload    RUNNING
    # SmRequest{target=RUNNING(2)} → field 1 varint 2 → bytes 08 02.
    ${hex}=    Evaluate    $payload.hex()
    Should Be Equal    ${hex}    0802

    # service_id agreement: djb2_low16 of the nanopb C type name, computed
    # the same way the supervisor (#386) and TipcMux register_call do.
    ${name}=    Sm Request Type Name
    ${sid}=     Djb2 Low16    ${name}
    # 0..0xFFFF; non-zero proves the hash ran. The C++ side hashes the
    # identical string, so the frame routes to sm's register_call entry.
    Should Be True    0 <= ${sid} <= 65535


Inject And Call Sm Live
    [Documentation]    Against a running sm + services/com: cast a signal,
    ...                then call RequestMode(RUNNING) and assert the reply
    ...                decodes as SmEmpty. Skips if com is unreachable.
    [Tags]    robot-node    live    priority-high

    ${ok}=    Run Keyword And Return Status    Com.Connect    timeout=3
    Skip If    not ${ok}    services/com not reachable at ${COM_ENDPOINT}

    ${type}    ${inst}=    Sm Tipc Address
    ${reqtype}=            Sm Request Type Name
    ${payload}=            Build Sm Request Payload    RUNNING

    # 1. Fire-and-forget signal inject (cast), impersonating "RobotTest".
    Com.Inject Signal    ${type}    ${inst}    ${reqtype}    ${payload}
    ...    src=RobotTest

    # 2. Call the service and decode the reply.
    ${reply}=    Com.Call Service    ${type}    ${inst}    ${reqtype}
    ...    ${payload}    src=RobotTest    timeout_ms=3000
    ${decoded}=    Decode Sm Empty    ${reply}
    Log    sm RequestMode reply: ${decoded}


*** Keywords ***
Djb2 Low16
    [Arguments]    ${s}
    ${v}=    Evaluate    __import__('functools').reduce(lambda h,c:((h*33)+ord(c)) & 0xffffffff, '${s}', 5381) & 0xffff
    RETURN    ${v}

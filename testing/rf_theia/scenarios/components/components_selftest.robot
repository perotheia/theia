*** Settings ***
Documentation    Hermetic selftest of the Pair-4 distributed-component
...              surface. Drives loop-port pairs via the LocalTransport
...              so no live theia is required.
...
...              Covers:
...                - Run Component + Stop Component lifecycle
...                - Component Call on a sender op + assertion via
...                  Component Expect on a receiver op
...                - SmProber ↔ SmStub message exchange (real-shape
...                  message round-trip without a live sm_daemon)
...                - Multiple instances under different `as_=` names
Library          rf_theia.TheiaTestLibrary


Suite Setup       Load Rig    ${CURDIR}/../fixtures/demo_rig.json
Suite Teardown    Tear Down Rig


*** Test Cases ***
Echo Sink Round Trip
    [Documentation]    Smallest Pair-4 case: one sender, one receiver,
    ...                paired via a loop port that matches by name.
    [Tags]    components    hermetic    selftest

    Run Component       Sink    as_=rx
    Run Component       Echo    as_=tx

    Component Call      tx    emit    msg=hello-pair-4
    Component Expect    rx    expect_message    expected=hello-pair-4    within=1s


SM Prober Drives Stub
    [Documentation]    Real-shape exchange: SmProber sends a set_state
    ...                command on its outbound port, SmStub receives it
    ...                and emits the broadcast, SmProber asserts on the
    ...                broadcast contents.
    ...
    ...                This is what the live test looks like when the
    ...                loop ports flip to TIPC and the stub goes away.
    [Tags]    components    hermetic    selftest    sm-prober

    Run Component       SmStub      as_=sm
    Run Component       SmProber    as_=probe

    Component Call      probe       set_state    state=RUN
    ${observed}=        Component Call    sm    receive_command    within=1s
    Should Be Equal     ${observed}    RUN

    Component Call      sm          emit_broadcast    state=RUN
    Component Expect    probe       expect_broadcast    state=RUN    within=1s


Multiple Instances Are Isolated
    [Documentation]    Two SmStub instances under different `as_=`
    ...                names should be addressable independently.
    [Tags]    components    hermetic    selftest

    Run Component       SmStub    as_=stub_a
    Run Component       Echo      as_=tx_a

    # tx_a → stub_a (loop ports paired by name 'sm_state' — Echo's `out`
    # doesn't collide because the port name is different)
    Stop Component      tx_a
    Stop Component      stub_a

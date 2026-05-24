*** Settings ***
Documentation    Pair-4 live scenario: co-located SmProber on
...              central_host drives sm_daemon over TIPC and asserts
...              the broadcast lands within budget.
...
...              In Pair 4 v1 this requires SSH transport + TIPC port
...              binding, neither of which is implemented. The test
...              is acting as spec for the next milestone: when
...              SSHTransport + TIPC bind land, this scenario passes
...              against a live theia stack with sm running.
Library          rf_theia.TheiaTestLibrary

Suite Setup       Load Rig    %{RIG_JSON=${CURDIR}/../fixtures/demo_rig.json}
Suite Teardown    Tear Down Rig


*** Test Cases ***
SmProber Drives Live SM Restart
    [Documentation]    Co-located SmProber on central_host issues a
    ...                set_state command to sm_daemon and asserts the
    ...                broadcast carries the new state within 2s.
    [Tags]    components    live    priority-high

    Run Component       SmProber    on=central_host    as_=probe

    Component Call      probe       set_state    state=RUN
    Component Expect    probe       expect_broadcast    state=RUN    within=2s

    Verdict             pass

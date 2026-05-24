*** Settings ***
Documentation    Hermetic selftest of the Pair-3 supervision-graph
...              surface. Drives synthetic supervisor_child_* events
...              through the testkit publisher so the test passes
...              without a live theia supervisor.
...
...              Covers:
...                - Load Supervision parses executor.yaml
...                - Assert Restart Order against derived (tree-based)
...                  and explicit expected sequences
...                - Assert Healthy against an observed RUNNING child
...                - Assert Restart Within Limit
Library          rf_theia.TheiaTestLibrary
Library          rf_theia.testkit.event_publisher.SupervisorEventPublisher


Suite Setup       Run Keywords
...                   Load Rig             ${CURDIR}/../fixtures/demo_rig.json
...                   AND   Load Supervision    ${CURDIR}/../fixtures/central_host_executor.yaml
Suite Teardown    Tear Down Rig


*** Test Cases ***
Rest For One Restarts Crashed Child And Successors
    [Documentation]    Crash 'crypto' under core_sup [rest_for_one] and
    ...                assert the observed restart-start order matches
    ...                the OTP-expected sequence: crypto, sm,
    ...                network_sup, host_svc_sup, pltf_sup.
    [Tags]    supervision    hermetic    selftest

    # Initial steady-state: all children RUNNING.
    Publish Child Running    exec
    Publish Child Running    core
    Publish Child Running    crypto
    Publish Child Running    sm

    # Anchor the observation window at Crash Child time.
    Crash Child    crypto

    # Simulate supervisor's restart cascade (the events a real
    # supervisor would publish via the watcher).
    Publish Child Starting   crypto         restart_count=1
    Publish Child Starting   sm             restart_count=1
    Publish Child Starting   network_sup    restart_count=1
    Publish Child Starting   host_svc_sup   restart_count=1
    Publish Child Starting   pltf_sup       restart_count=1

    # Expected order derived from the supervision tree
    # (parent core_sup has rest_for_one strategy).
    Assert Restart Order


One For One Restarts Only Crashed Child
    [Documentation]    Crash 'com' under network_sup [one_for_one] and
    ...                assert only com restarts (not its siblings).
    [Tags]    supervision    hermetic    selftest

    Publish Child Running    nm
    Publish Child Running    com
    Publish Child Running    osi

    Crash Child    com
    Publish Child Starting   com    restart_count=1

    Assert Restart Order


Explicit Order Matches Override
    [Documentation]    Bypass the tree-derivation by passing an explicit
    ...                expected sequence to Assert Restart Order.
    [Tags]    supervision    hermetic    selftest

    Crash Child    sm
    Publish Child Starting    sm             restart_count=1
    Publish Child Starting    network_sup    restart_count=1

    Assert Restart Order    sm    network_sup


Healthy Reached Within Window
    [Documentation]    Assert Healthy passes when the named child
    ...                reaches RUNNING after Crash Child anchored the
    ...                observation window.
    [Tags]    supervision    hermetic    selftest

    Crash Child    com
    Publish Child Starting   com    restart_count=2
    Publish Child Running    com    restart_count=2    pid=999

    Assert Healthy    com    within=2s


Restart Count Within Limit
    [Documentation]    With max_restarts=3 on network_sup, two restarts
    ...                of com is still within the limit.
    [Tags]    supervision    hermetic    selftest

    Publish Child Running    com    restart_count=2

    Assert Restart Within Limit    com

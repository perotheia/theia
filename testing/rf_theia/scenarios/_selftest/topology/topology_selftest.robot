*** Settings ***
Documentation    Hermetic selftest of the Pair-5 topology surface.
...              Uses the consistent fixture pair:
...                fixtures/demo_only_rig.json     (only compute_host)
...                fixtures/demo_netgraph.json     (Demo3Way wiring)
...
...              No live SUT required — Pair 5 is static-graph analysis.
...              The cross-check returns clean against the consistent
...              pair; the rig-audit scenario drives the inconsistent
...              real rig.
Library          rf_theia.TheiaTestLibrary


Suite Setup       Run Keywords
...                   Load Rig         ${CURDIR}/../../fixtures/demo_only_rig.json
...                   AND   Load Topology    ${CURDIR}/../../fixtures/demo_netgraph.json
Suite Teardown    Tear Down Rig


*** Test Cases ***
CounterNode Broadcasts GetReply To Both Subscribers
    [Documentation]    CounterNode.GetReply has two destinations in the
    ...                Demo3Way wiring: DriverNode (the caller) and
    ...                ObserverNode (the observer). Order-insensitive.
    [Tags]    topology    hermetic    selftest

    Assert Routes To    CounterNode    GetReply    DriverNode    ObserverNode


Driver And Incrementer Both Source Inc
    [Documentation]    Both DriverNode and IncrementerNode emit Inc to
    ...                CounterNode. Assert from each sender's POV.
    [Tags]    topology    hermetic    selftest

    Assert Routes To    DriverNode         Inc    CounterNode
    Assert Routes To    IncrementerNode    Inc    CounterNode


Reachability Transitively Closes
    [Documentation]    DriverNode → CounterNode → ObserverNode is a
    ...                two-hop reachability path. Assert it walks.
    [Tags]    topology    hermetic    selftest

    Assert Reachable        DriverNode    CounterNode
    Assert Reachable        DriverNode    ObserverNode


Ticker Node Is Isolated
    [Documentation]    TickerNode has no signals declared (it's a clock
    ...                with side effects elsewhere). Reachability from
    ...                TickerNode must be empty.
    [Tags]    topology    hermetic    selftest

    Assert Not Reachable    TickerNode    CounterNode
    Assert Not Reachable    TickerNode    DriverNode


Consistent Fixtures Pass The Cross Check
    [Documentation]    demo_only_rig.json (one machine, three demo
    ...                components) and demo_netgraph.json (the matching
    ...                three compositions) must validate cleanly.
    [Tags]    topology    hermetic    selftest

    Assert Netgraph Matches Rig
    # TickerNode has no signals — a warning, intentional. Confirm the
    # cross-check returned exactly one warning and no errors.
    ${issues}=    Get Topology Issues
    Length Should Be    ${issues}    1
    Should Contain      ${issues}[0]    silent_node


Strict Mode Surfaces Warnings As Failures
    [Documentation]    With severity=warning, the silent_node warning
    ...                becomes a failure. Confirms strict-mode hygiene
    ...                gate works as advertised.
    [Tags]    topology    hermetic    selftest

    Run Keyword And Expect Error    *silent_node*
    ...    Assert Netgraph Matches Rig    severity=warning


Wrong Destination Set Is Caught
    [Documentation]    CounterNode emits GetReply to TWO destinations.
    ...                A "Routes To one destination only" claim should
    ...                fail with a useful message.
    [Tags]    topology    hermetic    selftest    negative

    Run Keyword And Expect Error    *routes to*expected*
    ...    Assert Routes To    CounterNode    GetReply    DriverNode

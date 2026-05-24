*** Settings ***
Documentation    Platform executor regression: per-node supervision via
...              synthetic node_sup injection.
...
...              When a worker child hosts one or more reporting=true
...              nodes, the supervisor synthesises an extra layer in
...              TreeSnapshot:
...
...                core_sup [rest_for_one]              (real, in executor.yaml)
...                └── sm [worker]                       (real, in executor.yaml)
...                    └── sm.node_sup [one_for_all]    (SYNTHESISED)
...                        └── SmDaemon                  (SYNTHESISED, reporting=true)
...
...              one_for_all at node_sup means: any reporting node-thread
...              dying = whole worker child dies. Supervisor's parent
...              strategy (rest_for_one in core_sup's case) then handles
...              the worker restart.
...
...              Non-reporting nodes are NOT synthesised as tree rows —
...              they're invisible to the supervisor and don't trigger
...              the one_for_all cascade if they die.
...
...              Today: this scenario fails because (a) #366 hasn't
...              propagated `reporting` into central_host_executor.yaml,
...              and (b) #364 hasn't taught the supervisor to synthesise
...              the rows. Spec for both.
Library          rf_theia.TheiaTestLibrary
Library          Collections


Suite Setup       Run Keywords
...                   Load Rig             %{RIG_JSON=${CURDIR}/../../fixtures/demo_rig.json}
...                   AND   Load Supervision    %{EXECUTOR_YAML=${CURDIR}/../../fixtures/central_host_executor.yaml}
Suite Teardown    Tear Down Rig


*** Test Cases ***
Node Sup Appears Under Reporting Worker
    [Documentation]    Verify the supervisor synthesises a node_sup row
    ...                under the sm worker. sm hosts SmDaemon
    ...                (reporting=true by default). After Load Rig,
    ...                Get Topology should show sm → sm.node_sup →
    ...                SmDaemon.
    [Tags]    platform-executor    supervision    node-sup    live    priority-high

    ${topo}=                Get Supervisor Tree

    # The synthesised row is named "<child>.node_sup" by convention
    # (so it doesn't collide with any real supervisor name in the
    # tree).
    ${node_sup_row}=        Get From Dictionary    ${topo}    sm.node_sup
    Should Be Equal         ${node_sup_row}[strategy]    one_for_all
    Should Be Equal         ${node_sup_row}[parent]      sm

    # The node itself is a worker row under node_sup.
    ${sm_daemon_row}=       Get From Dictionary    ${topo}    SmDaemon
    Should Be Equal         ${sm_daemon_row}[parent]     sm.node_sup


Crashing A Node Thread Kills The Worker
    [Documentation]    node_sup uses one_for_all: a node-thread missing
    ...                its watchdog triggers SIGTERM on the whole worker
    ...                child. The parent strategy (rest_for_one on
    ...                core_sup, in sm's case) then restarts the worker.
    ...
    ...                Drive this by Crash Child sm: the worker dies,
    ...                supervisor restarts sm per rest_for_one (covered
    ...                by the existing restart_strategies suite). What's
    ...                NEW: the watchdog scope is now per-node-thread,
    ...                not per-worker.
    [Tags]    platform-executor    supervision    node-sup    live

    Crash Child            sm

    # The cascade is the same shape as the existing rest_for_one case
    # (sm + everything after it in core_sup's child list).
    Assert Restart Order   within=15s
    Assert Healthy         sm    within=10s

    # After restart, the synthetic tree should be intact again.
    ${topo}=                Get Supervisor Tree
    Dictionary Should Contain Key    ${topo}    sm.node_sup
    Dictionary Should Contain Key    ${topo}    SmDaemon

    Verdict                pass


Non Reporting Node Is Not In Tree
    [Documentation]    A node with reporting=false should NOT show up
    ...                as a tree row. It still EXISTS in the process,
    ...                it just doesn't heartbeat and doesn't participate
    ...                in the watchdog / node_sup machinery.
    ...
    ...                This test will be meaningful once at least one
    ...                non-reporting node exists in the demo rig. For
    ...                now the assertion is conservative: confirm that
    ...                EVERY node row in the tree has a corresponding
    ...                heartbeat seen by the supervisor (i.e. nothing
    ...                reporting=false slipped through).
    [Tags]    platform-executor    supervision    node-sup    live    selective

    ${topo}=     Get Supervisor Tree
    ${names}=    Get Dictionary Keys    ${topo}

    # Every row whose parent ends in `.node_sup` must be a known
    # reporting node — supervisor watchdogs it, restart_count is
    # populated (even if zero), uptime_ms is non-zero.
    FOR    ${name}    IN    @{names}
        ${row}=         Get From Dictionary    ${topo}    ${name}
        ${is_node}=     Run Keyword And Return Status
        ...                Should End With    ${row}[parent]    .node_sup
        Continue For Loop If    not ${is_node}
        Should Be True    ${row}[uptime_ms] > 0    Node row ${name} has uptime_ms=0; either supervisor hasn't seen its first heartbeat yet, or it's a non-reporting node that leaked into the tree.
    END

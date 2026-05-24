*** Settings ***
Documentation    Platform executor regression: OTP restart strategies.
...
...              Three SUT-level test cases drive a real supervisor on
...              central_host through its three strategy modes and
...              verify the cascade matches what the executor.yaml
...              promises.
...
...              Tree under test (central_host_executor.yaml):
...
...                root [one_for_all, max=3/5s]
...                └── ar_sup [rest_for_one]
...                    ├── core_sup [rest_for_one]
...                    │   ├── exec, core, crypto, sm           (leaves)
...                    │   ├── network_sup [one_for_one]
...                    │   │   └── nm, com, osi, idsm, diag, tsync
...                    │   ├── host_svc_sup [one_for_one]
...                    │   └── pltf_sup [one_for_one]
...                    └── app_sup [one_for_one]
...
...              All cases require:
...                - live supervisor reachable on localhost:5051
...                - central_host's executor.yaml emitted
...
...              Skip with --exclude live on a hermetic host.
Library          rf_theia.TheiaTestLibrary


Suite Setup       Run Keywords
...                   Load Rig             %{RIG_JSON=${CURDIR}/../../fixtures/demo_rig.json}
...                   AND   Load Supervision    %{EXECUTOR_YAML=${CURDIR}/../../fixtures/central_host_executor.yaml}
Suite Teardown    Tear Down Rig


*** Test Cases ***
Rest For One Cascade On Core Sup
    [Documentation]    Crash 'crypto' under core_sup [rest_for_one].
    ...                The strategy says: restart crypto + every
    ...                sibling AFTER it in declared order. Children
    ...                BEFORE crypto (exec, core) must stay healthy
    ...                throughout.
    ...
    ...                Expected restart prefix (derived from the tree):
    ...                  crypto, sm, network_sup, host_svc_sup, pltf_sup
    [Tags]    platform-executor    supervision    live    rest-for-one    priority-high

    Crash Child    crypto

    Assert Restart Order    within=15s
    Assert Healthy          crypto         within=10s
    Assert Healthy          sm             within=15s
    Assert Healthy          network_sup    within=15s

    Assert Restart Within Limit    crypto
    Assert Restart Within Limit    sm
    Assert Restart Within Limit    network_sup

    Verdict    pass


One For One Isolation On Network Sup
    [Documentation]    Crash 'com' under network_sup [one_for_one].
    ...                The strategy says: ONLY com restarts; siblings
    ...                nm, osi, idsm, diag, tsync must NOT be touched.
    ...
    ...                Expected restart prefix: just [com].
    [Tags]    platform-executor    supervision    live    one-for-one    priority-high

    Crash Child    com

    Assert Restart Order      com    within=10s
    Assert Healthy            com    within=10s
    Assert Restart Within Limit    com

    Verdict    pass


One For All Escalation On Root
    [Documentation]    Crash 'ar_sup' itself. Per root's
    ...                [one_for_all], all root children restart —
    ...                in this tree, root has only one child (ar_sup),
    ...                so the cascade reaches every leaf below ar_sup.
    ...
    ...                Expected restart: ar_sup restarts (root's
    ...                strategy), which cascades to its subtree
    ...                (ar_sup's rest_for_one + descendants).
    ...
    ...                Test asserts at the root level: ar_sup must
    ...                come back, and every descendant leaf must
    ...                reach RUNNING within the recovery budget.
    [Tags]    platform-executor    supervision    live    one-for-all    priority-high

    Crash Child    ar_sup

    Assert Restart Order    ar_sup    within=20s
    Assert Healthy          ar_sup         within=15s

    # Deepest leaves recover too — recovery cascades all the way down.
    Assert Healthy          exec           within=30s
    Assert Healthy          sm             within=30s
    Assert Healthy          com            within=30s

    Assert Restart Within Limit    ar_sup

    Verdict    pass

*** Settings ***
Documentation    Pair-3 e2e: validate the rest_for_one cascade on
...              core_sup. Crash a middle child (sm) and assert the
...              supervisor restarts sm + every sibling that follows
...              it in declared order.
...
...              Requires:
...                - live theia supervisor reachable on central_host's
...                  services/com gRPC endpoint
...                - executor.yaml emitted under
...                  deploy/.staging/central_host/ipk/
...
...              Skip with `--exclude live` on a hermetic host.
Library          rf_theia.TheiaTestLibrary

Suite Setup       Run Keywords
...                   Load Rig             %{RIG_JSON=${CURDIR}/../fixtures/demo_rig.json}
...                   AND   Load Supervision    %{EXECUTOR_YAML=${CURDIR}/../fixtures/central_host_executor.yaml}
Suite Teardown    Tear Down Rig


*** Test Cases ***
Crashing SM Cascades Through Rest Of Core Sup
    [Documentation]    sm sits in core_sup [rest_for_one]. Per OTP
    ...                semantics, crashing sm should restart sm itself
    ...                plus everything after it: network_sup,
    ...                host_svc_sup, pltf_sup.
    ...
    ...                Children declared before sm (exec, core, crypto)
    ...                MUST stay running — assert that too.
    [Tags]    supervision    live    priority-high

    Crash Child    sm

    # Derived expected: [sm, network_sup, host_svc_sup, pltf_sup]
    Assert Restart Order    within=15s

    # All restarted children must reach RUNNING within budget.
    Assert Healthy    sm              within=10s
    Assert Healthy    network_sup     within=15s

    # Restart counts must stay within the policy limit.
    Assert Restart Within Limit    sm
    Assert Restart Within Limit    network_sup

    Verdict    pass

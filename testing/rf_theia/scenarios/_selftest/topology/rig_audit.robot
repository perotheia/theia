*** Settings ***
Documentation    Pair-5 audit scenario: compare the FULL demo rig
...              (3 machines, ~23 components) against the demo
...              netgraph (Demo3Way wiring only — central_host's
...              services are emitted from separate .art files).
...
...              This SHOULD fail today — and that's the point. The
...              cross-check surfaces ~20 platform components whose
...              compositions live outside demo_netgraph.json,
...              telling us the rig audit is broader than any single
...              netgraph file. The next step is to merge per-package
...              netgraphs into a rig-wide one (artheia task,
...              not rf-theia).
...
...              Acts as spec for the rig-wide netgraph emit.
Library          rf_theia.TheiaTestLibrary


Suite Setup       Run Keywords
...                   Load Rig         ${CURDIR}/../../fixtures/demo_rig.json
...                   AND   Load Topology    ${CURDIR}/../../fixtures/demo_netgraph.json
Suite Teardown    Tear Down Rig


*** Test Cases ***
Full Demo Rig Audit Surfaces Platform Service Mismatches
    [Documentation]    Expected to FAIL until artheia emits a rig-wide
    ...                netgraph that includes the platform services'
    ...                compositions. The failure message lists every
    ...                missing composition — that's the spec for the
    ...                next artheia gen-netgraph improvement.
    [Tags]    topology    rig-audit    expected-fail

    Run Keyword And Expect Error    *cross-check failed*
    ...    Assert Netgraph Matches Rig
    ${issues}=    Get Topology Issues
    Log Many      @{issues}
    # At least one error of the expected kind.
    Should Match     ${issues}[0]    *rig_composition_missing*

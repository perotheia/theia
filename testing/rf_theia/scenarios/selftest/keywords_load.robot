*** Settings ***
Documentation    Selftest: TheiaTestLibrary loads cleanly and every keyword
...              resolves. Runs hermetically — no live supervisor, no
...              trace stream. Catches grammar drift, missing imports,
...              and adapter mis-wiring before they bite real scenarios.
Library          rf_theia.TheiaTestLibrary


*** Test Cases ***
Library Imports
    [Documentation]    Smoke: the library instantiates without reaching
    ...                for any of the lazy-loaded adapters.
    [Tags]    selftest    hermetic
    Log    rf-theia loaded


TPT Engine Wires
    [Documentation]    Vendored TPT engine accepts partitions, signals,
    ...                and a transition. No `Run Time Engine` call —
    ...                that would actually run the partition.
    [Tags]    selftest    hermetic    tpt
    Create Partition    idle
    Create Partition    active
    Set Signal          rpm    0
    Add Transition      idle    active    rpm > 500


T Wait Works
    [Documentation]    Sanity-check the duration parser.
    [Tags]    selftest    hermetic
    T Wait    100ms

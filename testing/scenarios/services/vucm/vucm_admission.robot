*** Settings ***
Documentation    VUCM update-admission gate — the AUTOSAR go/no-go conjunction a
...              vehicle must satisfy before a campaign fans RequestUpdate, driven
...              LIVE over TIPC against a running VucmGate (services/vucm).
...
...              This is a COMPOSITION e2e, not a unit test: a unit test on
...              admission_denied() checks the conjunction math, but only this
...              proves the from_sm/from_nm/from_phm receivers are wired, that a
...              cast updates gate state, and that CheckForCampaign then BLOCKS or
...              ADMITS per the conjunction:
...                authorize = SM(parked) ∧ NM(link ok) ∧ PHM(healthy) ∧ window
...
...              The probe casts SmStateMsg / NmStatusMsg / PhmHealthStatus (the
...              three foreign admission edges sm/nm/phm broadcast) then calls
...              CheckForCampaign; the BLOCK/PASS verdict is read from VucmGate's
...              own log (/tmp/theia/vucm.log). Each test uses its own campaign_id
...              and resets it after, so a re-checking neighbour can't cross-talk.
...
...              Requires: a live services rig with VucmGate ENFORCING
...              (deploy/config/central/vucm.json → enforce_sm/nm/phm = 1).
...              Attach to the running rig; never owns the supervisor. Skip with
...              --exclude live on a hermetic host.

Library          vucm_admission_lib.py
Force Tags       live    services    vucm

*** Test Cases ***
Moving Vehicle Blocks An Update
    [Documentation]    SM not RUNNING (vehicle moving/updating) → admission BLOCKED.
    Set Vehicle Network State    OPERATIONAL
    Set Vehicle Health           OK
    Set Vehicle SM State         STARTING
    Start Campaign               campaign_id=rf-sm-block
    Admission Should Block       rf-sm-block    SM state
    [Teardown]    Reset Campaign    rf-sm-block

Tunnel Blocks An Update
    [Documentation]    NM DEGRADED (tunnel / bad link) → admission BLOCKED even
    ...                with the vehicle parked and healthy.
    Set Vehicle SM State         RUNNING
    Set Vehicle Health           OK
    Set Vehicle Network State    DEGRADED
    Start Campaign               campaign_id=rf-nm-block
    Admission Should Block       rf-nm-block    NM state
    [Teardown]    Reset Campaign    rf-nm-block

Unhealthy Vehicle Blocks An Update
    [Documentation]    PHM DEGRADED (unhealthy) → admission BLOCKED.
    Set Vehicle SM State         RUNNING
    Set Vehicle Network State    OPERATIONAL
    Set Vehicle Health           DEGRADED
    Start Campaign               campaign_id=rf-phm-block
    Admission Should Block       rf-phm-block    PHM level
    [Teardown]    Reset Campaign    rf-phm-block

Parked Healthy Vehicle Admits The Update
    [Documentation]    The full conjunction satisfied — SM RUNNING ∧ NM
    ...                OPERATIONAL ∧ PHM OK → admission PASSES: the campaign
    ...                advances past AUTHORIZING to INSTALLING (fans RequestUpdate).
    Set Vehicle SM State         RUNNING
    Set Vehicle Network State    OPERATIONAL
    Set Vehicle Health           OK
    Start Campaign               campaign_id=rf-admit
    Admission Should Pass        rf-admit
    [Teardown]    Reset Campaign    rf-admit

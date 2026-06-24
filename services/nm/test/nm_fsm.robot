*** Settings ***
Documentation    NM config-transaction gen_statem (NmCfgTxn) — drive the
...              two-phase-commit lifecycle via artheia.probe, assert the
...              transitions AND the FSM data via the STATEM trace.
...
...              NmCfgTxn is the network-config two-phase commit FSM:
...                STEADY ──Txn{AddWifi,RemoveWifi,SetVpn,SetAutoConn}──▶ PENDING
...                PENDING ──Txn{Confirm,Abort,Timeout}──▶ STEADY
...
...              It takes NO wire messages (the statem-never-off-the-wire
...              rule). NmCfgGate (tipc 0x8001002F) drives it in-process: the
...              production path is the NmCfgIf CALL ops (SetVpn/Confirm, each
...              returns a txn_state reply); the gate ALSO has a `txn_in`
...              receiver that accepts the same Txn* events as CASTS (no reply)
...              and post_event()s them into the FSM — the SmGate/LifecycleIn
...              idiom. NmCfgTester (a probe-only sender, in no composition)
...              casts at that receiver to drive the FSM standalone:
...
...                rf → probe (NmCfgTester sender) → TIPC cast → NmCfgGate
...                txn_in → handle_cast → post_event → NmCfgTxn FSM →
...                STATEM trace → observer → bus →
...                Wait For Statem State / Assert Statem Data.
...
...              Same generic hybrid-automata keywords as sm_fsm.robot — only
...              the node names + .art differ, proving the statem harness
...              generalizes across FCs.
...
...              NmCfgTxn's FSM `data` is NmCfgTxnData{committed, pending,
...              confirm_left_s, note}. on_enter stamps `note` with the
...              transition tag (apply / settle / steady) and sets
...              confirm_left_s to the confirm window (30) in PENDING, 0
...              elsewhere — both asserted below.
...
...              Prereq: binaries built + staged (Start Statem Stack runs the
...              workspace `apps/stage_local.sh`, which must stage nm — i.e. a
...              services-bearing rig such as @rig_zonal / odd_path_local, NOT
...              the demo apps rig). The suite stages + runs the supervisor; it
...              does NOT build. Tag 'live' (runs where the services stack
...              stages; dry-run validates the keyword surface anywhere).

Library          rf_theia.TheiaTestLibrary

Suite Setup      Start Statem Stack    node=nm_cfg_txn    gate=NmCfgGate
...              tester=NmCfgTester    art=system/services/nm/package.art
Suite Teardown   Stop Statem Stack

Force Tags       nm    fsm    statem    live


*** Test Cases ***
NM Config Txn Applies Then Confirms (STEADY To PENDING To STEADY)
    [Documentation]    The happy path of the two-phase commit. A SetVpn
    ...                change moves the FSM STEADY→PENDING (config applied,
    ...                confirm window armed); a Confirm commits it back to
    ...                STEADY. Assert each transition + the FSM data: `note`
    ...                tags the transition and confirm_left_s is the live
    ...                window in PENDING, 0 after settle.

    # TxnSetVpn: STEADY → PENDING. on_enter stamps note=apply and arms the
    # confirm window (confirm_left_s = 30s).
    Emit Statem Event         TxnSetVpn
    ${pending}=    Wait For Statem State     PENDING       within=2s
    Assert Statem Data        note=apply
    Should Be True    ${pending}[data][confirm_left_s] > 0
    ...    PENDING carries no confirm window: ${pending}[data]

    # TxnConfirm: PENDING → STEADY. on_enter stamps note=settle and clears
    # the confirm window (confirm_left_s = 0 outside PENDING).
    Emit Statem Event         TxnConfirm
    ${steady}=     Wait For Statem State     STEADY        within=2s
    Assert Statem Data        note=settle
    Should Be Equal As Integers    ${steady}[data][confirm_left_s]    0
    ...    confirm window not cleared on settle: ${steady}[data]


NM Config Txn Aborts Back To STEADY (Rollback Path)
    [Documentation]    The rollback path. From STEADY (left by the first
    ...                test), a TxnAddWifi applies → PENDING; a TxnAbort
    ...                rolls back → STEADY. Proves the FSM returns to STEADY
    ...                on the abort edge (not only on confirm), with the
    ...                confirm window cleared.

    # TxnAddWifi: STEADY → PENDING.
    Emit Statem Event         TxnAddWifi
    ${pending}=    Wait For Statem State     PENDING       within=2s
    Assert Statem Data        note=apply

    # TxnAbort: PENDING → STEADY (rollback). note=settle, window cleared.
    Emit Statem Event         TxnAbort
    ${steady}=     Wait For Statem State     STEADY        within=2s
    Assert Statem Data        note=settle
    Should Be Equal As Integers    ${steady}[data][confirm_left_s]    0
    ...    confirm window not cleared on abort: ${steady}[data]


NM Config Txn Auto Rolls Back On Timeout
    [Documentation]    The safety property the two-phase commit exists for:
    ...                if the operator never confirms, the FSM auto-rolls-back
    ...                on the confirm-window timeout. Drive PENDING then inject
    ...                TxnTimeout (the same edge the gate's confirm timer fires)
    ...                and assert the FSM settles back to STEADY.

    Emit Statem Event         TxnSetAutoConn
    Wait For Statem State     PENDING       within=2s

    # TxnTimeout: PENDING → STEADY (auto-rollback).
    Emit Statem Event         TxnTimeout
    ${steady}=     Wait For Statem State     STEADY        within=2s
    Assert Statem Data        note=settle

*** Settings ***
Documentation    OTA config-migration, end to end and LIVE against etcd-backed per.
...
...              A config's shape evolves (CounterConfig v1 {step,max,wrap,label}
...              → v2 +hysteresis, an add-field migration). Its stored value in
...              per/etcd, tagged with the v1 shape digest, must be rewritten to
...              the v2 digest by the migration — and a rollback must restore v1.
...
...              This runs the ON-DEVICE overlay path: `theia-migrate forward`
...              does Snapshot(pre-<artifact>) + PerManager.MigrateBulk over TIPC
...              (per dlopen's the plugin .so), the same call UcmGate's EvInstalled
...              runs for a UCM-adopted update; `theia-migrate rollback` does
...              RestoreSnapshot (the config rollback a PHM-unhealthy update fires
...              alongside the SW rollback). The assertion is the etcd KEYSPACE:
...              the stored shape digest flips v1 → v2 and back.
...
...              Requires: the composer rig (a real cluster etcd — per aborts
...              without one, so this CANNOT run on a bare boxter), a v1 config
...              seeded, and a built migration plugin + migration.json in
...              CM_MIGRATION_DIR. See the fixture / session notes for setup.

Library          config_migration_lib.py
Force Tags       live    services    ota    migration    needs-etcd

*** Test Cases ***
Migration Rewrites The Config Shape V1 To V2
    [Documentation]    Seeded v1 config → theia-migrate forward → the etcd value's
    ...                shape digest is now v2, and the add-field default landed.
    Config Digest Should Be V1
    Migrate Forward
    Config Digest Should Be V2
    Config Should Contain      counter

Rollback Restores The V1 Shape
    [Documentation]    theia-migrate rollback → RestoreSnapshot → the etcd value's
    ...                shape digest is back to v1 (the config rollback that pairs
    ...                with an SW rollback on a failed update).
    Migrate Rollback
    Config Digest Should Be V1

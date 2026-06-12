*** Settings ***
Documentation    Config-migration tests — PER NODE, parametrized by (node,
...              config_type, v1/v2 shapes, rules) in per_migration_lib.py's
...              CASES list. Each node carries its OWN v1->v2 evolution and rule
...              set:
...                - counter   : ADD a field (hysteresis)
...                - observer  : RENAME a field (name->tag, same number)
...                - demo_fsm  : RENAME + ADD (the P4 consolidation target)
...
...              Hermetic cases (default) run the REAL tools/migrate/migrate.py
...              + a nanopb decode round-trip that mirrors the dlopen'd plugin —
...              no live stack. The `live` case also drives per MigrateBulk and
...              asserts online == offline (the lockstep invariant).
...
...              Run hermetic:
...                PYTHONPATH=. ../.venv/bin/robot --include hermetic \
...                  rf_theia/scenarios/services/per_migration/
...              Run live (needs `theia start` + etcd):
...                PYTHONPATH=. ../.venv/bin/robot --include live \
...                  rf_theia/scenarios/services/per_migration/
Library          ${CURDIR}/per_migration_lib.py


*** Test Cases ***
Counter Config Migrates: add hysteresis
    [Documentation]    CounterConfig v1 {step,max_value,wrap,label} -> v2 adds
    ...                hysteresis (add-field rule). Offline migrate.py result +
    ...                nanopb round-trip both yield the expected v2 value.
    [Tags]    migration    hermetic    counter
    Assert Digest Bumped        counter
    Migrate Offline             counter
    Assert Migrated Value       counter
    Assert Nanopb Roundtrip     counter

Observer Config Migrates: rename name to tag
    [Documentation]    ObserverConfig v1 {poll_ms,name} -> v2 renames name->tag
    ...                (same field number 2 — the value is carried, not copied).
    [Tags]    migration    hermetic    observer
    Assert Digest Bumped        observer
    Migrate Offline             observer
    Assert Migrated Value       observer
    Assert Nanopb Roundtrip     observer

P4 Config Migrates: rename + add (consolidation)
    [Documentation]    P4Config v1 {timeout_s,tag} -> v2 {timeout_s,label,step,
    ...                poll_ms,amount}: rename tag->label + add step/poll_ms/
    ...                amount. The cross-node consolidation target.
    [Tags]    migration    hermetic    demo_fsm
    Assert Digest Bumped        demo_fsm
    Migrate Offline             demo_fsm
    Assert Migrated Value       demo_fsm
    Assert Nanopb Roundtrip     demo_fsm

Lockstep Online Equals Offline (P4)
    [Documentation]    LIVE: seed P4Config v1 into the running per, build + load
    ...                the plugin, MigrateBulk, snapshot, and assert the online
    ...                (nanopb plugin) result equals the offline (migrate.py)
    ...                result — the end-to-end lockstep invariant. Needs the
    ...                stack up (theia start) + etcd + the //migration plugin.
    [Tags]    migration    live    demo_fsm
    Migrate Online And Compare    demo_fsm

*** Settings ***
Documentation    Mender → UCM on-device install hand-off, end to end and LIVE.
...
...              The production OTA loop is: Mender pushes a .mender → the
...              theia-swp module stages releases/<ver> + flips `current` → the
...              ArtifactInstall_Leave state-script hands the release to the
...              on-device UCM agent (theia-migrate adopt → UcmDaemon.
...              RequestUpdate) → UCM runs the AUTOSAR lifecycle.
...
...              rf-theia has no Mender server and doesn't need one to exercise
...              the ON-DEVICE chain. The Mender adapter (rf_theia.adapters.mender)
...              reproduces exactly what Mender's update module does to disk and
...              which state-script it fires, so this asserts the whole hand-off
...              live: stage → deliver → adopt → UCM accepts → current advances,
...              plus rollback.
...
...              Requires: a live rig with UcmDaemon reachable and
...              $THEIA_ROOT/bin/theia-migrate present (the native adopter that
...              ships in theia-runtime). Set MENDER_THEIA_ROOT to the install
...              prefix. Attach to the running rig; never owns the supervisor.

Library          mender_adopt_lib.py
Force Tags       live    services    ota    mender

*** Test Cases ***
Mender Delivers A Release And UCM Adopts It
    [Documentation]    The full on-device chain: stage a (free-swap) release,
    ...                deliver it the way Mender does, and UCM adopts it over
    ...                TIPC — current advances to the delivered version.
    Stage Release              2.0.0
    Adopt Should Succeed       2.0.0
    Current Release Should Be  2.0.0

A Second Delivery Advances Current
    [Documentation]    Idempotent, repeatable: a second Mender install lands and
    ...                UCM adopts it too.
    Stage Release              2.1.0
    Adopt Should Succeed       2.1.0
    Current Release Should Be  2.1.0

Rollback Restores The Previous Release
    [Documentation]    Mender ArtifactRollback: after a delivery, restoring
    ...                `current` re-aims it at the prior release.
    Stage Release              3.0.0
    Mender Deliver             3.0.0
    Current Release Should Be  3.0.0
    Mender Rollback
    Current Release Should Be  2.1.0

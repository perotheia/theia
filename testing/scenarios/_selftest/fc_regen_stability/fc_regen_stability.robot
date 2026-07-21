*** Settings ***
Documentation    Regen-stability guard: every committed FC's lib +
...              main + impl/BUILD.bazel MUST equal what `artheia
...              gen-fc` emits today. Hand-edits to
...              generated files are forbidden — they break the
...              gen → build dependency chain and silently rot.
...
...              The only file that's allowed to drift is the
...              user's hand-edited impl handler
...              (services/<short>/impl/<Daemon>_handlers.cc); the
...              BUILD.bazel beside it IS checked.
...
...              If this test fails, the fix is NOT to commit the
...              hand-edit. The fix is to:
...
...                1. Move the .art spec to express the change.
...                2. Re-run gen-fc and commit the regen.
...
...                Or — if the template itself is wrong — fix the
...                template in artheia/generators/templates/fc_app/
...                and re-run gen-fc across all FCs.
...
...              Drives the daemon FCs (sm, com, per, ucm, log) AND
...              the non-services FCs (apps×3, gateway) —
...              gen-fc stays byte-stable wherever the FC lives.
Library          ${CURDIR}/fc_regen_lib.py


Suite Setup      Anchor At Workspace


*** Variables ***
${WORKSPACE}     ${CURDIR}/../../../..


*** Keywords ***
Anchor At Workspace
    ${ws_abs}=    Evaluate
    ...    str(__import__('pathlib').Path(r"${WORKSPACE}").resolve())
    Use Workspace    ${ws_abs}


*** Test Cases ***
sm regenerates byte-identically
    [Documentation]    services/sm/{lib,main,impl/BUILD.bazel} ==
    ...                gen-fc output from services/system/sm/package.art.
    [Tags]    fc-regen-stability    hermetic    selftest
    Regen And Diff FC    sm


com regenerates byte-identically
    [Documentation]    services/com/{lib,main,impl/BUILD.bazel} ==
    ...                gen-fc output from services/system/com/package.art.
    [Tags]    fc-regen-stability    hermetic    selftest
    Regen And Diff FC    com


per regenerates byte-identically
    [Documentation]    services/per/{lib,main,impl/BUILD.bazel} ==
    ...                gen-fc output from services/system/per/package.art.
    [Tags]    fc-regen-stability    hermetic    selftest
    Regen And Diff FC    per


ucm regenerates byte-identically
    [Documentation]    services/ucm/{lib,main,impl/BUILD.bazel} ==
    ...                gen-fc output from services/system/ucm/package.art.
    [Tags]    fc-regen-stability    hermetic    selftest
    Regen And Diff FC    ucm


log regenerates byte-identically
    [Documentation]    services/log/{lib,main,impl/BUILD.bazel} ==
    ...                gen-fc output from services/log/system/package.art.
    ...                Note log uses the new per-#368 spec layout
    ...                (spec under services/log/system/, not
    ...                services/system/log/); the canonical TraceCollector
    ...                node is here.
    [Tags]    fc-regen-stability    hermetic    selftest
    Regen And Diff FC    log


# Non-services (consuming-workspace) regen coverage lives in the user-flow
# harness now: ci/run.sh s2 + ci/test/gen_chain.robot scaffold a FRESH
# workspace per run — no committed reference tree to drift (demo/ retired).


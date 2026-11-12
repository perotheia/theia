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
Library          OperatingSystem


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


rdsdemo producer regenerates byte-identically
    [Documentation]    services/rds/demo/RdsProducer/{lib,main,impl/BUILD.bazel} ==
    ...                gen-fc --composition RdsProducer output from
    ...                system/services/rdsdemo/package.art (the rds writer).
    [Tags]    fc-regen-stability    hermetic    selftest
    Regen And Diff FC    rdsdemo_producer


rdsdemo consumer regenerates byte-identically
    [Documentation]    services/rds/demo/RdsConsumer/{lib,main,impl/BUILD.bazel} ==
    ...                gen-fc --composition RdsConsumer output. Its readers
    ...                declare `from=cam_left.frames` / `from=cam_right.frames`,
    ...                so they must open the two WRITER prototypes' instances.
    [Tags]    fc-regen-stability    hermetic    selftest
    Regen And Diff FC    rdsdemo_consumer
    ${r}=    Get File    ${CURDIR}/../../../../services/rds/demo/RdsConsumer/lib/FrameConsumer.hh
    Should Contain    ${r}    "/Cam_left/frames"
    Should Contain    ${r}    "/Cam_right/frames"


rdsdemo writer streams are keyed per prototype at runtime
    [Documentation]    RdsProducer prototypes FrameProducer TWICE. main.cc must
    ...                construct BOTH and name each (set_node_name), and the
    ...                writer accessor must key on that runtime name, so the two
    ...                prototypes publish on two streams, not one.
    [Tags]    fc-regen-stability    hermetic    selftest
    ${m}=    Get File    ${CURDIR}/../../../../services/rds/demo/RdsProducer/main/main.cc
    Should Contain    ${m}    cam_left.set_node_name("cam_left");
    Should Contain    ${m}    cam_right.set_node_name("cam_right");
    ${h}=    Get File    ${CURDIR}/../../../../services/rds/demo/RdsProducer/lib/FrameProducer.hh
    Should Contain    ${h}    ::ara::rds::owner_instance(rds_node_name_, "frames")

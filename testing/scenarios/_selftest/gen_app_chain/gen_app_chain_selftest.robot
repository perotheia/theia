*** Settings ***
Documentation    End-to-end selftest of `artheia gen-app --kind fc`
...              and its three-slice (lib / main / impl) regen
...              policy. The test exercises the contract that
...              hand-edits in impl/ survive every regen mode
...              except --force.
...
...              Synthetic FC: services/duo/ with a 2-node .art
...              (PingerNode plain + PongerNode statem). The
...              suite fixture wipes the FC before AND after so a
...              failed run leaves no in-tree artifacts.
...
...              Stages:
...
...              1. Initial emit — every slice gets written.
...              2. Idempotent re-emit — lib + main overwrite,
...                 impl preserved (default policy).
...              3. User hand-edit on impl/handlers.cc survives a
...                 re-emit byte-for-byte.
...              4. --force overwrite — impl/ gets replaced;
...                 lib + main unchanged from a content standpoint.
...              5. Add new signal to .art → re-emit → lib +
...                 proto reflect the new message, impl untouched.
...              6. Bazel build — `bazel build
...                 //services/duo/main:duo` succeeds, proving the
...                 lib + main + impl + proto pipeline links.
...
...              Stage 6 is tagged 'live-bazel' so a CI without
...              bazel can skip it.
Library          ${CURDIR}/gen_app_chain_lib.py
Library          OperatingSystem


Suite Setup      Set Up Workspace + Synthetic FC
Suite Teardown   Tear Down Synthetic FC


*** Variables ***
${WORKSPACE}     ${CURDIR}/../../../..


*** Keywords ***
Set Up Workspace + Synthetic FC
    ${ws_abs}=    Evaluate
    ...    str(__import__('pathlib').Path(r"${WORKSPACE}").resolve())
    Use Workspace    ${ws_abs}
    # Make sure no stale duo/ from a previous failed run.
    Wipe Synthetic FC
    Seed FC Source

Tear Down Synthetic FC
    # Tear down Bazel mutations first (some tests may have skipped).
    Run Keyword And Ignore Error    Unwire FC From Bazel
    Wipe Synthetic FC


*** Test Cases ***
Stage 1 — initial emit writes all three slices
    [Documentation]    First gen-app run emits lib/ (DaemonClass.hh
    ...                + netgraph + BUILD), main/ (main.cc + BUILD),
    ...                impl/ (handlers + BUILD), and the .proto
    ...                under proto-out. Validates the gen-app
    ...                template-set hasn't lost a file.
    [Tags]    gen-app-chain    hermetic    selftest    stage-1

    Run Gen App

    # Lib slice — every file carries AUTO-GENERATED.
    ${lib_h}=    Slice File Exists    lib    PingerNode.hh
    File Has Autogen Marker    ${lib_h}
    ${lib_n}=    Slice File Exists    lib    PingerNode_netgraph.hh
    File Has Autogen Marker    ${lib_n}
    # Log.hh (#383) — emitted from `tag = "..."` in the .art, with
    # node-name fallback when omitted. The duo .art doesn't declare
    # a tag yet, so we expect the fallback (node name).
    ${lib_log}=    Slice File Exists    lib    Log.hh
    File Has Autogen Marker    ${lib_log}
    File Contains    ${lib_log}    kLogTag
    ${lib_b}=    Slice File Exists    lib    BUILD.bazel
    File Has Autogen Marker    ${lib_b}

    # Main slice — also AUTO-GENERATED.
    ${main_c}=    Slice File Exists    main    main.cc
    File Has Autogen Marker    ${main_c}
    ${main_b}=    Slice File Exists    main    BUILD.bazel
    File Has Autogen Marker    ${main_b}

    # Impl slice — user-owned, no AUTO-GENERATED banner.
    ${impl_c}=    Slice File Exists    impl    PingerNode_handlers.cc
    File Lacks Autogen Marker    ${impl_c}
    Slice File Exists    impl    BUILD.bazel


Stage 2 — re-emit overwrites lib + main, preserves impl
    [Documentation]    The contract: re-running gen-app without
    ...                --force regenerates lib + main (idempotent
    ...                — bytes don't actually change for a stable
    ...                .art) but SKIPS impl. Asserts impl bytes
    ...                survive byte-for-byte.
    [Tags]    gen-app-chain    hermetic    selftest    stage-2

    ${impl_path}=    Slice File Exists    impl    PingerNode_handlers.cc
    ${impl_h0}=    File Hash    ${impl_path}
    ${lib_path}=    Slice File Exists    lib    PingerNode.hh
    ${lib_h0}=    File Hash    ${lib_path}

    Run Gen App

    ${impl_h1}=    File Hash    ${impl_path}
    ${lib_h1}=    File Hash    ${lib_path}

    # Impl untouched.
    Should Be Equal As Strings    ${impl_h0}    ${impl_h1}
    ...    msg=impl was clobbered by re-emit without --force

    # Lib byte-for-byte identical (idempotency invariant — same
    # .art, same .ns → same bytes; if this fails, the generator
    # is non-deterministic).
    Should Be Equal As Strings    ${lib_h0}    ${lib_h1}
    ...    msg=lib emission is non-deterministic


Stage 3 — hand-edit on impl survives re-emit
    [Documentation]    Append a unique user comment to
    ...                impl/handlers.cc, re-run gen-app without
    ...                --force, assert the comment is still there.
    ...                This is the workflow that matters most: the
    ...                user iterates on handler logic while
    ...                regenerating the public API from the .art.
    [Tags]    gen-app-chain    hermetic    selftest    stage-3

    ${impl}=    Slice File Exists    impl    PingerNode_handlers.cc
    Append User Comment To Handler    ${impl}    stage3-survives-regen

    Run Gen App

    File Contains    ${impl}    USER-EDIT MARKER: stage3-survives-regen


Stage 4 — --force overwrites impl
    [Documentation]    --force is the escape hatch when the user
    ...                wants the template's fresh impl skeleton
    ...                (e.g., after refactoring a .art node's port
    ...                set). Asserts the user marker is gone.
    [Tags]    gen-app-chain    hermetic    selftest    stage-4

    ${impl}=    Slice File Exists    impl    PingerNode_handlers.cc
    File Contains    ${impl}    USER-EDIT MARKER: stage3-survives-regen

    Run Gen App With Force

    File Does Not Contain    ${impl}    USER-EDIT MARKER


Stage 5 — add signal to .art → proto + lib regen
    [Documentation]    Mutate the .art to add a new senderReceiver
    ...                + a sender port on PingerNode. Re-emit and
    ...                assert:
    ...                  - the new message lands in the .proto
    ...                  - the new interface lands in lib/
    ...                  - impl/ stays untouched (user handlers
    ...                    weren't affected — port signatures are
    ...                    additive, no break)
    [Tags]    gen-app-chain    hermetic    selftest    stage-5

    Add Signal To Art    Notifs    NotifMsg

    ${impl_before}=    File Hash    ${WORKSPACE}/services/duo/impl/PingerNode_handlers.cc

    Run Gen App

    ${proto}=    Set Variable    ${WORKSPACE}/platform/proto/system/services/duo/duo.proto
    File Contains    ${proto}    NotifMsg
    File Contains    ${proto}    message NotifMsg

    ${lib_h}=    Slice File Exists    lib    PingerNode.hh
    File Contains    ${lib_h}    NotifMsg

    ${impl_after}=    File Hash    ${WORKSPACE}/services/duo/impl/PingerNode_handlers.cc
    Should Be Equal As Strings    ${impl_before}    ${impl_after}
    ...    msg=impl drifted after a non-breaking .art change


Stage 6 — bazel build the synthetic FC
    [Documentation]    Wire the new FC's proto into
    ...                platform/proto:platform_protos, run
    ...                `bazel build //services/duo/main:duo`,
    ...                assert it links. Cleans up the platform_protos
    ...                edit on teardown.
    ...
    ...                This is the strongest single check: it
    ...                proves the lib/main/impl/proto chain
    ...                emitted by gen-app produces a real, linkable
    ...                cc_binary against platform/runtime.
    [Tags]    gen-app-chain    live-bazel    selftest    stage-6

    Wire FC Into Bazel
    Bazel Build FC

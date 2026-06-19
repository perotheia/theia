*** Settings ***
Documentation    Hermetic selftest of `artheia gen-autosar-system`
...              regen consistency.
...
...              The vendor AUTOSAR PSP repo
...              (vendor/autosar/vehicle_gen2_cmp_psp/) is regenerated
...              from external DBC + FIBEX exports by three generators:
...                gen-autosar-system  — emits the per-bus .art
...                gen-can-codec       — emits .proto + .c encoders
...                gen-fibex-codec     — emits .proto + .c decoders
...
...              Only gen-autosar-system has a `--package` flag (the
...              other two emit code, not .art). When the workspace
...              switched to the `system.*` namespace (see
...              docs/tasks/TODO/autosar-regen-package-names.md), the
...              emitted package name went from
...                `autosar.vehicle_gen2_cmp_psp.system`
...              to
...                `system.autosar.vehicle_gen2`
...              — and the CLI help example fell out of date for a
...              while. This selftest catches regressions in the
...              `--package` plumbing and asserts the generator stays
...              byte-deterministic so re-running it on unchanged
...              inputs doesn't churn the vendor repo.
...
...              Hermetic: drives `artheia gen-autosar-system` over
...              two tiny synthetic catalog.json files (3 PDUs each).
...              No vendor DBC/FIBEX needed; runs in a single Bazel /
...              CI step.
Library          ${CURDIR}/autosar_regen_lib.py
Library          OperatingSystem


Suite Setup    Set Up Synthetic Catalogs In    ${TMPDIR}/autosar_regen_selftest


*** Variables ***
${TMPDIR}    %{TMPDIR=/tmp}


*** Test Cases ***
Package Decl Reflects --package Flag
    [Documentation]    The `--package system.autosar.vehicle_gen2` flag
    ...                must land verbatim in the emitted .art file's
    ...                `package <name>` line. If this fails the
    ...                generator stopped honoring the flag.
    [Tags]    autosar-regen    hermetic    selftest    priority-high

    ${path}=    Run Gen Autosar System    out1.art    system.autosar.vehicle_gen2
    ${pkg}=     Read Package Decl From Art    ${path}
    Should Be Equal As Strings    ${pkg}    system.autosar.vehicle_gen2


Different --package Produces Different Decl
    [Documentation]    Negative-style check that the package decl
    ...                isn't hardcoded. Passing a different value
    ...                must show up in the emit.
    [Tags]    autosar-regen    hermetic    selftest

    ${path}=    Run Gen Autosar System    out2.art    test.pkg.other_namespace
    ${pkg}=     Read Package Decl From Art    ${path}
    Should Be Equal As Strings    ${pkg}    test.pkg.other_namespace


Regen Is Byte Deterministic
    [Documentation]    Idempotency: running the generator twice with
    ...                identical inputs MUST produce byte-identical
    ...                output. A diff here means we're about to start
    ...                churning the vendor PSP repo on every CI run
    ...                — usually from dict-iteration order or a leaked
    ...                timestamp.
    [Tags]    autosar-regen    hermetic    selftest    priority-high

    ${a}=    Run Gen Autosar System    determinism_a.art    system.autosar.vehicle_gen2
    ${b}=    Run Gen Autosar System    determinism_b.art    system.autosar.vehicle_gen2
    Files Are Byte Identical    ${a}    ${b}


Catalog PDUs Flow Into Interface Decls
    [Documentation]    Smoke-check the catalog→.art wiring. Catalog
    ...                lists PDU "FOO_01" → .art must contain
    ...                `interface senderReceiver FOO_01_Iface`. Catches
    ...                generator regressions where messages drop.
    [Tags]    autosar-regen    hermetic    selftest

    ${path}=    Run Gen Autosar System    pdu_check.art    system.autosar.vehicle_gen2
    Art File Forward Declares Pdu    ${path}    FOO_01
    Art File Forward Declares Pdu    ${path}    ALPHA_01

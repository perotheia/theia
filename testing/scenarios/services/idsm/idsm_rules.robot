*** Settings ***
Documentation     IDSM rule-catalog v1 (userspace, no eBPF) — consistent reporting.
...
...               Wraps the security-plane unit tests so a rule-catalog
...               regression reports into Robot output alongside every other
...               scenario. The catalog (docs/autosar/services/idsm.md) is
...               realized WITHOUT eBPF for v1: ss/proc/sha256 + nft cover
...               Cat A/C/D/H (detect) and Cat B (fw nft-counter correlation);
...               fw enforces per-FC egress via `socket cgroupv2` over osi's
...               theia.slice/<fc> cgroups. eBPF is deferred to v2 (only the
...               short-lived-dial case it uniquely catches).
...
...               The C++ tests own WHAT is asserted (build them first:
...               bazel build //services/idsm/test:test_proc_detector
...                           //services/fw/test:test_egress_gen);
...               this suite reports their verdict. The full live loop
...               (place an FC in a slice, deny its egress, see the drop +
...               idsm Cat B) is exercised manually / by the live test binaries.
Library           ${CURDIR}/idsm_rules_lib.py

Force Tags        services-idsm    services-fw    security    hermetic


*** Test Cases ***
ProcDetector Classifies Cat A/C/D/H + Parses Cat-B Counters
    [Documentation]    ss/proc parse + the A-vs-C-vs-D classifier + the gRPC-port
    ...                scope + the Cat-B nft-counter JSON parse. A failure here
    ...                means a rule predicate or the fw-drop correlation broke.
    Run ProcDetector Test

Fw Generates Per-FC Egress Output Chain
    [Documentation]    fw's build_ruleset emits a valid `output` chain doing
    ...                per-FC `socket cgroupv2` allow-list + named idsm_b_<fc>
    ...                counter + log + drop; an unplaced FC is skipped.
    Run Fw Egress Gen Test

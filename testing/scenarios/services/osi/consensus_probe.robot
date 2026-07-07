*** Settings ***
Documentation     Cooperative-alert consensus probe (HANDOFF2).
...
...               The osi OsiV2v node (services/osi, tipc 0x800100A0) runs a
...               per-topic binary consensus via broadcast Gaussian BP: alert
...               beliefs (mu, lam, witness) ride the beacon, each receiver fuses
...               them, and the network agrees in a few rounds and re-converges
...               when witnesses change. The load-bearing decision is the
...               provenance cap (§5.1): a crowd's relayed echo can never outweigh
...               one direct observation.
...
...               This suite runs the canonical Python probe suite
...               (services/osi/test/consensus_probe.py). Two layers:
...
...               HERMETIC — a multi-node sim of the acceptance experiments
...               E1/E3/E5/E6/E7 (the SAME math the C++ AlertConsensus and its
...               //services/osi/test:test_consensus unit test implement). No FC,
...               no radios; always runs.
...
...               LIVE — inject AlertBelief-bearing beacons into the running osi
...               FC via the tdb TdbV2v client, REPLACING the Meshtastic radio
...               (which the deploy disables, run_on_start=false), and assert
...               GetAlertDecision converges. SKIPS when no stack is up.
Library           ${CURDIR}/consensus_probe_lib.py


*** Test Cases ***
Consensus Parity E1 To E7 (Hermetic)
    [Documentation]    The best-of-N consensus matches the tts reference on
    ...                convergence, majority, containment, loss, and the 5 s
    ...                deployable rate — the algorithm gate.
    [Tags]    services-osi    consensus    hermetic
    Run Consensus Parity Sim

Osi Consensus Converges Over TIPC (Live)
    [Documentation]    Drive the live osi FC by injecting witness beacons over
    ...                TIPC (the radio disabled) and confirm its decision flips
    ...                to the witnessed alert.
    [Tags]    services-osi    consensus    live    probe
    Require Osi V2v Listening
    Run Consensus Live Probe

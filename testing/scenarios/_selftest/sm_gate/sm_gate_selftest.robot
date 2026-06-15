*** Settings ***
Documentation    SM gate split + probe re-send (T1 + T2).
...
...              sm is two nodes: SmDaemon (the GenStateM, TIPC
...              0x8001000D) and SmGate (a plain receiver, TIPC
...              0x8001001D). External lifecycle messages hit the GATE,
...              which post_event()s them into the FSM in-process — the
...              statem never takes wire messages directly.
...
...              T1: the central supervisor, after all children are up,
...                  casts SystemBoot + StartupComplete to the gate. The
...                  gate forwards them and sm reaches RUNNING.
...
...              T2: a test PROBE re-sends StartupComplete to the gate
...                  over the services/com gRPC bridge, impersonating the
...                  supervisor. sm is already RUNNING, so the gate
...                  forwards it and the statem receives-and-ignores it
...                  (no transition out of RUNNING). Path:
...                  test → com gRPC InjectSignal → robot probe → TIPC
...                  → SmGate → post_event → SmDaemon FSM.
...
...              Prereq: binaries built (Bazel FCs + demo apps, CMake
...              supervisor + CMake services-com gRPC bridge). The suite
...              stages + runs them; it does NOT build. Tag 'live'.

Library          ${CURDIR}/sm_gate_lib.py

Suite Teardown   Stop Sm Gate Stack

Force Tags       selftest    live    sm    sm-gate


*** Test Cases ***
T1 Supervisor Drives SM To Running Via Gate
    [Documentation]    Central boots; the supervisor casts SystemBoot +
    ...                StartupComplete to the gate; the gate post_events
    ...                them into the FSM; sm reaches RUNNING.
    Stage And Start Central
    Sm Reached Running

T2 Probe Re-sends Startup Complete; SM Logs And Ignores
    [Documentation]    With sm already RUNNING, the probe re-sends
    ...                StartupComplete to the gate over the com gRPC
    ...                bridge. The gate forwards it (so the forward count
    ...                grows past T1's), and sm stays RUNNING — a repeated
    ...                StartupComplete has no transition out of RUNNING.
    Start Com Bridge
    ${sent}=    Probe Resend Startup Complete
    Should Be True    ${sent}    probe InjectSignal was not sent

    # T1 forwarded StartupComplete once; the probe re-send adds another.
    ${n}=    Gate Forwarded Count    at_least=2
    Should Be True    ${n} >= 2
    ...    expected >=2 gate StartupComplete forwards (T1 + probe), got ${n}

    Sm Still Running

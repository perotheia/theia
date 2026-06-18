*** Settings ***
Documentation     Diag DoIP/UDS e2e smoke — the ara::diag diagnostic gateway.
...
...               The Diag FC (services/diag) is the tester-facing diagnostic
...               gateway: DoipServer (a runnable, tipc 0x80010017) owns a
...               DoIP TCP server on ISO-13400 port 13400, decodes UDS
...               (ISO 14229) requests, and routes them to UdsRouter
...               (0x80010018) over TIPC. A SINGLE logical ECU address
...               terminates UDS at Theia for the whole gateway.
...
...               This suite drives the live FC over REAL DoIP (TCP/13400)
...               via the canonical Python smoke
...               (services/diag/test/diag_doip_smoke.py): routing
...               activation, then the UDS v1 set — 0x10 session control,
...               0x22 read identity DID (VIN) + the phm fault-log DIDs
...               (0xFD00+idx, 0xFDFF count), 0x2E write DID, 0x19 read DTC
...               (from phm health). It asserts the positive responses + NRCs.
...               Robot owns the uniform PASS/FAIL reporting; the smoke owns
...               WHAT is asserted.
...
...               DoIP framing is unit-tested at the wire level
...               (services/diag/impl/doip.hpp); this live suite proves the
...               server is up + the UDS dispatch routes end to end.
...
...               Needs the stack up (`theia start`); SKIPS cleanly otherwise.
Library           ${CURDIR}/diag_doip_lib.py

Force Tags        services-diag    live    doip    uds    e2e


*** Test Cases ***
Diag Serves UDS Over DoIP
    [Documentation]    Routing-activate + run the UDS v1 sequence over TCP/13400;
    ...                assert positive responses + NRCs. SKIP if diag isn't up.
    Require Diag Listening
    Run Diag DoIP Smoke

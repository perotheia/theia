*** Settings ***
Documentation     IDSM probe smoke — consistent reporting wrapper.
...
...               The IDS-Manager FC (services/idsm, tipc 0x8001000F) ingests
...               eBPF detections, normalizes each into a kind=SECURITY
...               TraceRecord, and spills it into the trace firehose
...               (log[trace] @ 0x80010013). On a host without eBPF the
...               detector graceful-degrades to IDS_UNAVAILABLE, but the FC
...               still serves GetIdsStatus over TIPC.
...
...               This suite RUNS the canonical Python smoke
...               (services/idsm/test/idsm_probe_smoke.py) and reports its
...               verdict into Robot output so an idsm regression surfaces
...               alongside every other scenario. The smoke test itself owns
...               WHAT is asserted (it drives the live FC via the tdb TdbIdsm
...               client); Robot owns the uniform PASS/FAIL reporting.
...
...               The deeper ingest→firehose→escalate pipeline is covered
...               hermetically by the unit test //services/idsm/test:test_ingest
...               (mock backend + the TraceSubmitter test-sink); this live
...               suite proves the FC is up + serving over the wire.
...
...               Needs the stack up (`theia start`); SKIPS cleanly otherwise.
Library           ${CURDIR}/idsm_probe_lib.py

Force Tags        services-idsm    live    probe


*** Test Cases ***
Idsm Serves GetIdsStatus Over TIPC
    [Documentation]    The live idsm FC answers GetIdsStatus via the tdb
    ...                TdbIdsm probe with a definite IdsState (UNAVAILABLE =
    ...                the honest no-eBPF degrade on the dev host). A
    ...                non-zero smoke exit (timeout / UNKNOWN state / wire
    ...                failure) fails this case with the captured output.
    Require Idsm Listening
    Run Idsm Probe Smoke

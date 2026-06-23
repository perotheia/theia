#!/usr/bin/env python3
"""Self-test the log[trace] PG migration end-to-end.

Pipeline under test:
  producer  --(SOCK_DGRAM submit TraceRecord)-->  TraceStreamPump (0x80010013)
            --(PG name-sequence multicast)-->  every observer that pg_joined

Drives it with the LIVE supervisor (PG allocator) + log[trace] (the pump):
  1. N-OBSERVER  — two TraceObservers pg_join the TraceRecord group; the
                   supervisor allocates each a distinct delivery instance under
                   the shared group type. A single submitted record is
                   PG-multicast → BOTH observers receive it (the 2-tracecat
                   all-receive case). Proves the migration kept multi-consumer.

A "producer" here is a raw SOCK_DGRAM sendto of [TheiaMsgHeader][TraceRecord
proto] to the pump's in_records address (0x80010013) — exactly what every FC's
Tracer::TraceSubmitter does.

Run with supervisor (0x80020001) + log[trace] (0x80010013) already up.
"""
import socket
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import wire                       # noqa: E402
from artheia.gen_server.probe.codec import Codec                # noqa: E402
from artheia.gen_server.probe.context import ArtheiaContext     # noqa: E402
from artheia.observer.observer import TraceObserver             # noqa: E402

LOG_ART = REPO / "system/services/log/component.art"
PROTO = REPO / "platform/proto"
PUMP_TYPE = 0x80010013          # TraceStreamPump in_records (Tracer submit addr)
_LOG_PKG = "system.services.log"
_RECORD = "system_services_log_TraceRecord"


def submit_record(codec: Codec, node_name: str, msg_type: str, corr: int) -> None:
    """sendto one TraceRecord to the pump — mimics Tracer::TraceSubmitter."""
    payload = codec.encode(_LOG_PKG, _RECORD,
                           node_name=node_name, dst="", msg_type=msg_type,
                           corr_id=corr, ts_ns=0, payload=b"", kind=0)
    hdr = wire.Header(msg_type=wire.MSG_GEN_CAST, proto_len=len(payload),
                      service_id=wire.service_id(_RECORD), correlation_id=0)
    frame = wire.frame(hdr, payload)
    s = socket.socket(socket.AF_TIPC, socket.SOCK_DGRAM)
    try:
        s.sendto(frame, (socket.TIPC_ADDR_NAME, PUMP_TYPE, 0, 0))
    finally:
        s.close()


def main() -> int:
    ctx = ArtheiaContext(str(LOG_ART), proto_root=str(PROTO))
    codec = ctx.codec
    rc = 0
    obs_a = obs_b = None
    try:
        print("== N-OBSERVER: two pg_join the TraceRecord group ==")
        obs_a = TraceObserver.from_log_art(str(LOG_ART), proto_root=str(PROTO))
        obs_b = TraceObserver.from_log_art(str(LOG_ART), proto_root=str(PROTO))
        obs_a.start()
        obs_b.start()
        print("   both observers joined; giving the pump a beat to see members")
        time.sleep(0.5)

        # A warmup record makes the pump resolve the group_type (lazy on the
        # first record); the supervisor CALL means the first 1-2 records can be
        # dropped before resolve completes. Then submit a burst — multicast is
        # best-effort (lossy), so send enough that at least one lands on each.
        submit_record(codec, node_name="warmup", msg_type="warmup", corr=0)
        time.sleep(0.3)
        for i in range(20):
            submit_record(codec, node_name="pg-test-node",
                          msg_type="system_app_Ping", corr=100 + i)
            time.sleep(0.03)

        got_a = [r for r in obs_a.records(timeout=2.0) if r.src == "pg-test-node"]
        got_b = [r for r in obs_b.records(timeout=2.0) if r.src == "pg-test-node"]
        print(f"   observer A received {len(got_a)} pg-test-node record(s)")
        print(f"   observer B received {len(got_b)} pg-test-node record(s)")
        if got_a:
            print("   sample A:", got_a[0].src, got_a[0].msg_type, got_a[0].corr_id)
        assert got_a, "N-OBSERVER: observer A received no pg-test-node records"
        assert got_b, "N-OBSERVER: observer B received no pg-test-node records"
        # Both saw the SAME multicast stream (same node + type).
        assert got_a[0].msg_type == "system_app_Ping" == got_b[0].msg_type
        print("   N-OBSERVER OK (both observers received the multicast stream)")

        print("\nTRACE PG MIGRATION TEST PASSED")
    except AssertionError as e:
        print("FAIL:", e); rc = 1
    except Exception as e:
        print("ERROR:", type(e).__name__, e); rc = 2
    finally:
        for o in (obs_a, obs_b):
            if o:
                try: o.stop()
                except Exception: pass
    return rc


if __name__ == "__main__":
    raise SystemExit(main())

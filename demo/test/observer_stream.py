#!/usr/bin/env python3
"""End-to-end test of artheia.observer over the real log[trace] hub.

Launches the real services/log binary, attaches a TraceObserver (which binds a
subscriber addr + calls TraceCtl.Subscribe, all resolved from the log .art),
emits a TraceRecord to the pump, and asserts the observer streams the decoded
record (header fields + JSON). This is the packaged replacement for the
hand-rolled trace_collector_fanout.py — proves artheia.observer end-to-end.

Run (after `bazel build --config=linux //services/log/main:log`):
    PATH="$PWD/.venv/bin:$PATH" python demo/test/observer_stream.py
"""
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.observer import TraceObserver               # noqa: E402
from artheia.gen_server.probe import wire                # noqa: E402
from artheia.gen_server.probe.codec import Codec         # noqa: E402
from artheia.gen_server.probe.transport import TipcClient  # noqa: E402

BINARY = REPO / "bazel-bin/services/log/main/log"
LOG_ART = REPO / "services/log/system/log/component.art"
PUMP_TIPC = 0x80010013
LOG_PKG = "system.services.log"


def emit_trace(codec, **fields):
    """Cast a TraceRecord to the pump, as a real node's Tracer would."""
    wirebytes = codec.encode(LOG_PKG, "system_services_log_TraceRecord", **fields)
    c = TipcClient(PUMP_TIPC, 0)
    if not c.connect():
        raise ConnectionError("cannot reach TraceStreamPump")
    hdr = wire.Header(msg_type=wire.MSG_GEN_CAST, proto_len=len(wirebytes),
                      service_id=wire.service_id("system_services_log_TraceRecord"),
                      correlation_id=0)
    c.send(wire.frame(hdr, wirebytes))
    c.close()


def main() -> int:
    if not BINARY.exists():
        print(f"missing {BINARY} — bazel build --config=linux "
              f"//services/log/main:log", file=sys.stderr)
        return 2

    proc = subprocess.Popen([str(BINARY)], env={"THEIA_LOG_LEVEL": "info"},
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    obs = None
    try:
        time.sleep(1.0)  # let pump + ctl bind

        obs = TraceObserver.from_log_art(str(LOG_ART),
                                         proto_root=str(REPO / "platform/proto"))
        obs.start()
        print(f"[obs] attached — subscriber 0x{obs._sub_type:08x}/{obs._sub_instance}")
        time.sleep(0.3)  # hub connects back

        # Emit two records (a cast in + a call in) toward the pump.
        emit_trace(obs.codec, node_name="CounterNode", dst="DriverNode",
                   msg_type="Inc", corr_id=7, ts_ns=111, kind=2)      # CAST_IN
        emit_trace(obs.codec, node_name="CounterNode", dst="ObserverNode",
                   msg_type="Get", corr_id=8, ts_ns=222, kind=4)      # CALL_IN
        print("[obs] emitted 2 TraceRecords to the pump")

        recs = []
        for rec in obs.records(timeout=2.0):
            recs.append(rec)
            print(f"[obs] record: node={rec.node_name!r} msg={rec.msg_type!r} "
                  f"kind={rec.kind} corr={rec.corr_id} json={rec.json}")
            if len(recs) >= 2:
                break

        ok = (len(recs) >= 2
              and recs[0].node_name == "CounterNode" and recs[0].msg_type == "Inc"
              and recs[0].kind == 2 and recs[0].corr_id == 7
              and recs[1].msg_type == "Get" and recs[1].kind == 4)
        json_ok = recs and '"msgType"' in recs[0].json
        print(f"[check] streamed {len(recs)} decoded records -> "
              f"{'PASS' if ok else 'FAIL'}")
        print(f"[check] JSON serialization present -> "
              f"{'PASS' if json_ok else 'FAIL'}")
        return 0 if (ok and json_ok) else 1
    finally:
        if obs:
            obs.stop()
        proc.terminate()
        proc.wait(timeout=3)


if __name__ == "__main__":
    sys.exit(main())

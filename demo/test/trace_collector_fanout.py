#!/usr/bin/env python3
"""End-to-end test of the trace hub (TraceStreamPump + TraceCtl) over real TIPC.

Launches the real services/log binary. An "observer" stand-in (built from the
probe's TIPC transport + lazy proto codec) does the full subscribe→receive loop:

  1. bind a subscriber TIPC address (TraceSubscriber type, distinct instance)
  2. call TraceCtl.Subscribe(sub_type, sub_instance) on the atomic control node
     -> the hub connects back to us and will fan out records
  3. emit a TraceRecord toward the pump (0x80010013, where every Tracer submits)
  4. confirm the pump put it in the ring + fanned it out to us

Proves: producer -> pump (raw, no decode) -> ring -> fan-out -> subscriber,
all internal TIPC, no gRPC.

Run (after `bazel build --config=linux //services/log/main:log`):
    PATH="$PWD/.venv/bin:$PATH" python demo/test/trace_collector_fanout.py
"""
import os
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import wire                     # noqa: E402
from artheia.gen_server.probe.codec import Codec              # noqa: E402
from artheia.gen_server.probe.transport import (              # noqa: E402
    TipcServer, TipcClient,
)

BINARY = REPO / "bazel-bin/services/log/main/log"

PUMP_TIPC = 0x80010013          # TraceStreamPump.in_records (Tracer submit addr)
CTL_TIPC  = 0x80010014          # TraceCtl (Subscribe server)
SUB_TYPE  = 0x8001001A          # observer's own service type
LOG_PKG   = "system.services.log"


def main() -> int:
    if not BINARY.exists():
        print(f"missing {BINARY} — run: bazel build --config=linux "
              f"//services/log/main:log", file=sys.stderr)
        return 2

    codec = Codec(str(REPO / "platform/proto"))
    sub_instance = os.getpid() & 0xFFFF

    # --- observer's receiver socket: where the hub fans records back to us ---
    received = []

    def on_frame(hdr, payload, conn):
        if hdr.service_id == wire.service_id("system_services_log_TraceRecord"):
            rec = codec.decode(LOG_PKG, "system_services_log_TraceRecord", payload)
            received.append(rec)

    observer = TipcServer(SUB_TYPE, sub_instance, on_frame)
    observer.start()
    print(f"[obs] subscriber bound (0x{SUB_TYPE:08x}, {sub_instance})")

    proc = subprocess.Popen([str(BINARY)], env={"THEIA_LOG_LEVEL": "info"},
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)  # let the pump + ctl bind

        # --- 2) Subscribe via TraceCtl ---
        sub_wire = codec.encode(
            LOG_PKG, "system_services_log_SubscribeReq",
            sub_type=SUB_TYPE, sub_instance=sub_instance, kind=0)
        ctl = TipcClient(CTL_TIPC, 0)
        if not ctl.connect():
            print("[FAIL] cannot reach TraceCtl"); return 1
        hdr = wire.Header(msg_type=wire.MSG_GEN_CALL,
                          proto_len=len(sub_wire),
                          service_id=wire.service_id("system_services_log_SubscribeReq"),
                          correlation_id=1)
        ctl.send(wire.frame(hdr, sub_wire))
        reply = ctl.recv_reply(timeout=3.0)
        print(f"[obs] Subscribe reply: {'ok' if reply else 'none'}")
        time.sleep(0.3)  # let the hub connect back to us

        # --- 3) emit a TraceRecord toward the pump ---
        rec_wire = codec.encode(
            LOG_PKG, "system_services_log_TraceRecord",
            node_name="CounterNode", dst="DriverNode", msg_type="Inc",
            corr_id=7, ts_ns=123456789, kind=2)  # kind=2 CAST_IN
        producer = TipcClient(PUMP_TIPC, 0)
        if not producer.connect():
            print("[FAIL] cannot reach TraceStreamPump"); return 1
        phdr = wire.Header(msg_type=wire.MSG_GEN_CAST,
                           proto_len=len(rec_wire),
                           service_id=wire.service_id("system_services_log_TraceRecord"),
                           correlation_id=0)
        producer.send(wire.frame(phdr, rec_wire))
        print("[obs] emitted TraceRecord{node=CounterNode msg=Inc kind=CAST_IN} to pump")

        # --- 4) confirm fan-out reached us ---
        deadline = time.monotonic() + 3.0
        while not received and time.monotonic() < deadline:
            time.sleep(0.05)

        if not received:
            print("[check] FAIL — no record fanned out to subscriber")
            return 1
        rec = received[0]
        print(f"[obs] received fanned-out record: "
              f"node={rec.get('node_name')!r} msg={rec.get('msg_type')!r} "
              f"kind={rec.get('kind')} corr={rec.get('corr_id')}")
        ok = (rec.get("node_name") == "CounterNode"
              and rec.get("msg_type") == "Inc"
              and rec.get("corr_id") == 7)
        print(f"[check] fan-out roundtrip -> {'PASS' if ok else 'FAIL'}")
        return 0 if ok else 1
    finally:
        observer.stop()
        proc.terminate()
        proc.wait(timeout=3)


if __name__ == "__main__":
    sys.exit(main())

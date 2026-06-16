#!/usr/bin/env python3
"""Smoke-drive the live shwa FC via artheia.probe.

Impersonates the tdb TdbShwa client (tipc 0x80020107 — distinct address, no
self-connect) to call GetAccelStatus over real TIPC: the hardware telemetry
(CPU/GPU/mem/temp/power/fan) the host backend derives on the central node. Run
with `shwa` listening (theia start).
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import ArtheiaContext  # noqa: E402

ART = REPO / "system/tools/tdb/tdb.art"
PROTO = REPO / "platform/proto"

_PM = {0: "UNKNOWN", 1: "MAXN", 2: "BALANCED", 3: "LOW"}


def main() -> int:
    ctx = ArtheiaContext(str(ART), proto_root=str(PROTO))
    probe = ctx.probe("TdbShwa").start()
    try:
        print("== GetAccelStatus ==")
        rep = probe.call("ShwaDaemon", "GetAccelStatus")
        print(f"  board={rep.get('board')!r} on_jetson={rep.get('on_jetson')} "
              f"power_mode={_PM.get(rep.get('power_mode', 0))}")
        print(f"  CPU {rep.get('cpu_util_pct')}% x{rep.get('cpu_count')} "
              f"@ {rep.get('cpu_freq_mhz')} MHz")
        print(f"  GPU {rep.get('gpu_util_pct')}% @ {rep.get('gpu_freq_mhz')} MHz")
        print(f"  MEM {rep.get('mem_used_mb')} / {rep.get('mem_total_mb')} MB")
        print(f"  temp={rep.get('temp_c')} C  power={rep.get('power_mw')} mW  "
              f"fan={rep.get('fan_rpm')} rpm")
        # Healthy if it reports a real board + a sane memory total.
        ok = bool(rep.get("board")) and rep.get("mem_total_mb", 0) > 0
        print("  smoke:", "OK" if ok else "NO TELEMETRY")
        return 0 if ok else 1
    finally:
        probe.stop()


if __name__ == "__main__":
    raise SystemExit(main())

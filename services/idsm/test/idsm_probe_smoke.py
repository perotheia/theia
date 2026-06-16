#!/usr/bin/env python3
"""Smoke-drive the live idsm FC via artheia.probe.

Impersonates the tdb TdbIdsm client (tipc 0x80020106 — a distinct address, so the
call doesn't self-connect) to query GetIdsStatus over real TIPC. On the dev host
there's no eBPF backend, so state=IDS_UNAVAILABLE (on_ebpf=False) unless a
mock_event_path is configured — the FC still serves status, proving the wire +
the graceful-degrade. Run with `idsm` listening (theia start).
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import ArtheiaContext  # noqa: E402

ART = REPO / "system/tools/tdb/tdb.art"
PROTO = REPO / "platform/proto"

_STATE = {0: "UNAVAILABLE", 1: "LOADED", 2: "DEGRADED"}


def main() -> int:
    ctx = ArtheiaContext(str(ART), proto_root=str(PROTO))
    probe = ctx.probe("TdbIdsm").start()
    try:
        print("== GetIdsStatus ==")
        rep = probe.call("IdsmDaemon", "GetIdsStatus")
        st = rep.get("state", 0)
        print(f"  state={_STATE.get(st, st)} on_ebpf={rep.get('on_ebpf')} "
              f"events={rep.get('events_total')} escalated={rep.get('escalated_total')} "
              f"last={rep.get('last_signature')!r}")
        # The FC is healthy if it serves a definite state. On the dev host that's
        # UNAVAILABLE (no eBPF) — the honest graceful-degrade.
        ok = st in (0, 1, 2)
        print("  smoke:", "OK" if ok else "BROKEN")
        return 0 if ok else 1
    finally:
        probe.stop()


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Smoke-drive the live nm FC via artheia.probe.

Impersonates NmPoller (a node in nm's own .art) to call NmDaemon's
GetNetworkStatus over real TIPC, proving the readiness FSM serves its current
state to clients. The FSM itself is driven by the in-process NmPoller reading
`ip` — so on a host with a carrier+address link this returns READY. Run with
`nm` already listening (theia start).
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import ArtheiaContext  # noqa: E402

ART = REPO / "system/services/nm/package.art"
PROTO = REPO / "platform/proto"

# NetState enum (mirrors the .art) for a readable print.
_STATE = {0: "DOWN", 1: "LINK_UP", 2: "READY", 3: "DEGRADED"}


def main() -> int:
    ctx = ArtheiaContext(str(ART), proto_root=str(PROTO))
    # Impersonate NmPoller (has its own address) to drive NmDaemon's ctl op.
    probe = ctx.probe("NmPoller").start()
    try:
        print("== GetNetworkStatus ==")
        rep = probe.call("NmDaemon", "GetNetworkStatus")
        st = rep.get("state", 0)
        print("  reply:", rep)
        print(f"  state={_STATE.get(st, st)} "
              f"iface={rep.get('interface') or '(auto)'} "
              f"carrier={rep.get('has_carrier')} addr={rep.get('has_address')}")
        # On this dev host enp0s31f6 has carrier + a global addr → READY.
        ok = st in (_STATE_OK := {1, 2})   # LINK_UP or READY = the FSM advanced
        print("  smoke:", "OK (FSM advanced past DOWN)" if ok else "DOWN (no link?)")
        return 0 if ok else 1
    finally:
        probe.stop()


if __name__ == "__main__":
    raise SystemExit(main())

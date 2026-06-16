#!/usr/bin/env python3
"""Smoke-drive the live osi FC via artheia.probe.

Impersonates a peer to call OsiCtl's GetResourceStatus over real TIPC, proving
the FC accounts for the running FCs' CPU%/RSS (read from /proc — works without
cgroup delegation). Then exercises SetResourceLimit (which graceful-degrades to
applied=false where the cgroup slice isn't delegated). Run with `osi` listening.
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import ArtheiaContext  # noqa: E402

# Drive osi through the canonical tdb CLIENT model: TdbOsi is a distinct client
# node (tipc 0x80020104) that CALLs OsiCtlIf. Impersonating OsiCtl itself would
# self-connect (TIPC load-balances a call to OsiCtl's own type across co-bound
# ports, including the probe's), so the caller MUST be a separate address.
ART = REPO / "system/tools/tdb/tdb.art"
PROTO = REPO / "platform/proto"


def main() -> int:
    ctx = ArtheiaContext(str(ART), proto_root=str(PROTO))
    probe = ctx.probe("TdbOsi").start()
    try:
        # (power mode moved to services/shwa; osi is pure cgroup/resource now.)
        print("== GetResourceStatus ==")
        rep = probe.call("OsiCtl", "GetResourceStatus")
        fcs = rep.get("fcs", []) or []
        print(f"  fc_count={rep.get('fc_count')}")
        # fcs entries are protobuf FcResource messages (attribute access).
        for r in fcs[:20]:
            print(f"    {r.fc:<8} pid={r.pid:<7} "
                  f"cpu={r.cpu_pct:6.2f}%  rss={r.rss_bytes // 1024:>7} KiB"
                  f"  cpu_max={r.cpu_max_pct}%  mem_high={r.mem_high}")
        # The stack has many FCs; accounting works if we saw at least a couple.
        ok = len(fcs) >= 2 and any(r.rss_bytes > 0 for r in fcs)
        print("  accounting:", "OK" if ok else "NO FCs ACCOUNTED")

        print("== SetResourceLimit(fc=tsync, cpu_max=50%, mem_high=128MiB) ==")
        rep = probe.call("OsiCtl", "SetResourceLimit",
                         fc="tsync", cpu_max_pct=50, mem_high=128 * 1024 * 1024)
        print("  reply:", rep)
        return 0 if ok else 1
    finally:
        probe.stop()


if __name__ == "__main__":
    raise SystemExit(main())

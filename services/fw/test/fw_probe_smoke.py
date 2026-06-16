#!/usr/bin/env python3
"""Smoke-drive the live fw FC via artheia.probe.

Impersonates the tdb TdbFw client (tipc 0x80020105 — a distinct address, so the
call doesn't self-connect) to query GetFirewallStatus + force ReloadRules over
real TIPC. With cap_net_admin (the postinstall setcap) the FC reports FW_APPLIED
with the generated rule count; without it, FW_DEGRADED (ruleset generated, not
installed). Run with `fw` listening (theia start).
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import ArtheiaContext  # noqa: E402

ART = REPO / "system/tools/tdb/tdb.art"
PROTO = REPO / "platform/proto"

_STATE = {0: "UNKNOWN", 1: "APPLIED", 2: "DEGRADED", 3: "DISABLED"}


def main() -> int:
    ctx = ArtheiaContext(str(ART), proto_root=str(PROTO))
    probe = ctx.probe("TdbFw").start()
    try:
        print("== GetFirewallStatus ==")
        rep = probe.call("FwDaemon", "GetFirewallStatus")
        st = rep.get("state", 0)
        print(f"  state={_STATE.get(st, st)} rules={rep.get('rule_count')} "
              f"overrides={rep.get('override_count')} msg={rep.get('message')!r}")

        print("== ReloadRules ==")
        rep = probe.call("FwDaemon", "ReloadRules")
        print("  reply:", rep)

        # The FC is healthy if it reports a definite state (APPLIED with rules, or
        # an honest DEGRADED on a host without cap_net_admin). UNKNOWN = broken.
        ok = st in (1, 2, 3) and (st != 1 or rep.get("applied"))
        print("  smoke:", "OK" if ok else "BROKEN (UNKNOWN state)")
        return 0 if ok else 1
    finally:
        probe.stop()


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Prove the ConfigUpdated fan-out is DEFERRED off the PutConfig reply path.

A subscriber pointed at a DEAD address would, under the old inline fan-out,
stall PutConfig for the full per-cast connect budget (250ms) before the caller
got its reply. With the deferred schedule_push, PutConfig returns immediately
and the (doomed) cast runs later on the node thread. So: subscribe a node that
maps to an UNREACHABLE addr, then time PutConfig — it must return in well under
the 250ms connect budget.

Run `per` with THEIA_NETGRAPH=services/per/test/per_netgraph_dead.json (which
maps dead_sub to an address nothing listens on).
"""
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import ArtheiaContext  # noqa: E402

ART = REPO / "system/services/per/package.art"
PROTO = REPO / "platform/proto"


def main() -> int:
    ctx = ArtheiaContext(str(ART), proto_root=str(PROTO))
    probe = ctx.probe_external(0x800100FE, 0, name="driver").start()
    try:
        # Subscribe a node whose netgraph addr is UNREACHABLE (dead_sub).
        probe.call("PerClient", "WatchConfig", target_node="cfgX",
                   subscriber_node="dead_sub", want_digest="")

        # Time the Put. With deferral the reply returns immediately; the doomed
        # cast (250ms connect to nothing) happens AFTER, off this path.
        t0 = time.monotonic()
        rep = probe.call("PerClient", "PutConfig", target_node="cfgX",
                         config=b"x" * 32, digest="v1", expect_rev=0)
        dt_ms = (time.monotonic() - t0) * 1000.0
        print(f"PutConfig reply in {dt_ms:.1f}ms: {rep}")
        # Generous ceiling: deferral should keep this ~single-digit ms. The old
        # inline path would be >=250ms (the dead-peer connect budget).
        ok = dt_ms < 150.0 and rep.get("status") == 0
        print("NON-BLOCKING:", "OK" if ok else "FAIL (reply waited on the cast)")
        return 0 if ok else 1
    finally:
        probe.stop()


if __name__ == "__main__":
    raise SystemExit(main())

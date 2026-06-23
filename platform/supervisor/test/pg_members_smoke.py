#!/usr/bin/env python3
"""Self-test the OTP pg:monitor PER-MEMBER broadcast path (the one all FCs use).

This exercises the path that `broadcast_members` (the regenerated FC broadcast)
relies on — distinct from the nameseq-multicast path proven by pg_multicast_smoke:

  producer.watch(group)            → PgWatch CALL; supervisor returns members +
                                     pushes PgMembership on every join/leave
  consumer_a.join(group)           → supervisor adds member, PUSHES the producer
  consumer_b.join(group)           → supervisor adds member, PUSHES the producer
  producer.members(group)          → {a, b} (the pushes refreshed the cache)
  producer.broadcast_members(...)  → cast to EACH member ([Pid ! Msg || ...])
  → BOTH consumers receive (the phm→sm / nm-status / sm-state shape).

Then consumer_b leaves → producer's member list drops to {a} (the {pg_leave}).

Uses HealthBeacon as a reachable wire type (supervisor's own proto). Run with the
supervisor (0x80020001) up (empty-tree executor.json is fine).
"""
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import ArtheiaContext        # noqa: E402
from artheia.gen_server.probe.pg import PgProbe            # noqa: E402

ART = REPO / "system/supervisor/component.art"
PROTO = REPO / "platform/proto"
GROUP = "system_supervisor_HealthBeacon"
GROUP_PKG = "system.supervisor"


def mk(ctx, name):
    p = ctx.probe("SupervisorWorker").start()
    pg = PgProbe(p, node_name=name)
    pg.arm_decode(GROUP, GROUP_PKG)
    return p, pg


def main() -> int:
    ctx = ArtheiaContext(str(ART), proto_root=str(PROTO))
    rc = 0
    probes, pgs = [], []

    def track(pair):
        probes.append(pair[0]); pgs.append(pair[1]); return pair[1]

    try:
        print("== producer watches the group (OTP pg:monitor) ==")
        prod = track(mk(ctx, "producer"))
        m0 = prod.watch(GROUP)
        print(f"   initial members: {len(m0)}")

        print("== two consumers join ==")
        ca = track(mk(ctx, "cons-a")); ra = ca.join(GROUP)
        cb = track(mk(ctx, "cons-b")); rb = cb.join(GROUP)
        assert int(ra.get("status", 1)) == 0 and int(rb.get("status", 1)) == 0
        ca.start_keepalive(0.5); cb.start_keepalive(0.5)

        # Give the supervisor's PgMembership pushes a beat to reach the producer.
        deadline = time.time() + 5.0
        while time.time() < deadline and len(prod.members(GROUP)) < 2:
            time.sleep(0.1)
        mem = prod.members(GROUP)
        print(f"   producer sees {len(mem)} members: {mem}")
        assert len(mem) >= 2, ("WATCH/PUSH: producer's member list did not reach 2 "
                               "after both consumers joined")
        print("   WATCH/PUSH OK")

        print("== producer broadcast_members → both consumers ==")
        n = prod.broadcast_members(GROUP, GROUP_PKG, GROUP,
                                   timestamp_ms=42, uptime_ms=1, generation=1,
                                   total_workers=2, active_workers=2)
        print(f"   cast to {n} member(s)")
        ga = ca.await_broadcast(GROUP, want=1, timeout=3.0)
        gb = cb.await_broadcast(GROUP, want=1, timeout=3.0)
        print(f"   consumer A got {len(ga)}, consumer B got {len(gb)}")
        assert ga and gb, "PER-MEMBER: not both consumers received the broadcast"
        assert ga[0].get("timestamp_ms") == 42
        print("   PER-MEMBER OK (both received the per-member cast)")

        print("== consumer B leaves → producer member list drops ==")
        cb.leave(GROUP)
        deadline = time.time() + 5.0
        while time.time() < deadline and len(prod.members(GROUP)) > 1:
            time.sleep(0.1)
        after = prod.members(GROUP)
        print(f"   producer now sees {len(after)} member(s)")
        assert len(after) == 1, "LEAVE: producer member list did not drop to 1"
        print("   LEAVE OK ({pg_leave} reached the producer)")

        print("\nPG PER-MEMBER (OTP pg:monitor) TEST PASSED")
    except AssertionError as e:
        print("FAIL:", e); rc = 1
    except Exception as e:
        print("ERROR:", type(e).__name__, e); rc = 2
    finally:
        for pg in pgs:
            try: pg.shutdown()
            except Exception: pass
        for p in probes:
            try: p.stop()
            except Exception: pass
    return rc


if __name__ == "__main__":
    raise SystemExit(main())

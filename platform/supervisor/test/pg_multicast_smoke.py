#!/usr/bin/env python3
"""Self-test the PG (process-group) TIPC name-sequence MULTICAST path end-to-end.

Drives the LIVE supervisor (the namespace authority) via artheia.probe's PgProbe:

  1. BASIC          — one probe joins a group; the supervisor allocates
                      {group_type, instance}; a broadcast reaches it.
  2. TWO-INSTANCE   — two probes join the SAME group; the supervisor hands each a
                      DISTINCT instance under the SAME group_type (the identity-
                      resolution answer: the supervisor allocates, no pid tricks).
  3. N-RECEIVE      — with N members joined, ONE broadcast datagram is delivered
                      to ALL N (the "3 tdb tracecat all-receive" case). Proves
                      name-sequence multicast (not anycast).
  4. RESOLVE-ONLY   — a pure broadcaster resolve()s the group_type WITHOUT taking
                      an instance, then broadcasts; existing members still receive.

The group name is the WIRE TYPE NAME (well-known, .art-derived) — here a stand-in
type the supervisor itself defines (system_supervisor_HealthBeacon) so the codec
is reachable without a full FC. The test asserts ONLY the PG allocator + multicast
semantics; it does not require the members to be real FCs.

Run with the supervisor already listening on 0x80020001 (bare/dev run binds inst 0).
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import ArtheiaContext        # noqa: E402
from artheia.gen_server.probe.pg import PgProbe            # noqa: E402

ART = REPO / "system/supervisor/component.art"
PROTO = REPO / "platform/proto"

# A wire type the supervisor's own proto defines → codec reachable for the test.
GROUP = "system_supervisor_HealthBeacon"
GROUP_PKG = "system.supervisor"


def _mk_pg(ctx, name):
    """A PgProbe backed by a started NodeProbe impersonating SupervisorCtl's peer.
    The probe identity address is irrelevant to PG (the supervisor allocates the
    group address); we only need its call_addr/cast_addr + codec."""
    probe = ctx.probe("SupervisorWorker").start()
    pg = PgProbe(probe, node_name=name)
    pg.arm_decode(GROUP, GROUP_PKG)   # make the group's wire type decodable
    return probe, pg


def main() -> int:
    ctx = ArtheiaContext(str(ART), proto_root=str(PROTO))
    rc = 0
    probes = []
    pgs = []

    def cleanup():
        for pg in pgs:
            try: pg.shutdown()
            except Exception: pass
        for p in probes:
            try: p.stop()
            except Exception: pass

    try:
        # ---- 1. BASIC -------------------------------------------------------
        print("== 1. BASIC: one join + one broadcast ==")
        p0, pg0 = _mk_pg(ctx, "pg-a"); probes.append(p0); pgs.append(pg0)
        rep0 = pg0.join(GROUP)
        print("   join reply:", rep0)
        assert int(rep0.get("status", 1)) == 0, "join failed"
        gtype = int(rep0["group_type"]); ginst0 = int(rep0["instance"])
        assert gtype != 0 and ginst0 != 0, "supervisor gave no addr"
        print(f"   allocated group_type=0x{gtype:08x} instance={ginst0}")

        # broadcast from a separate sender (resolve-only)
        ps, pgs_send = _mk_pg(ctx, "pg-send"); probes.append(ps); pgs.append(pgs_send)
        pgs_send.broadcast(GROUP, GROUP_PKG, GROUP,
                           timestamp_ms=111, uptime_ms=222, generation=1, total_workers=3, active_workers=3)
        got = pg0.await_broadcast(GROUP, want=1, timeout=3.0)
        print(f"   member received {len(got)} cast(s):", got[:1])
        assert len(got) >= 1, "BASIC: member did not receive the broadcast"
        print("   BASIC OK")

        # ---- 2. TWO-INSTANCE: distinct instances, same type -----------------
        print("== 2. TWO-INSTANCE: identity resolution ==")
        p1, pg1 = _mk_pg(ctx, "pg-b"); probes.append(p1); pgs.append(pg1)
        rep1 = pg1.join(GROUP)
        assert int(rep1.get("status", 1)) == 0
        gtype1 = int(rep1["group_type"]); ginst1 = int(rep1["instance"])
        print(f"   2nd member: group_type=0x{gtype1:08x} instance={ginst1}")
        assert gtype1 == gtype, "group_type MUST be the same for the same name"
        assert ginst1 != ginst0, "each member MUST get a DISTINCT instance"
        print("   TWO-INSTANCE OK (same type, distinct instances)")

        # ---- 3. N-RECEIVE: one broadcast → all members ----------------------
        print("== 3. N-RECEIVE: one datagram → all members ==")
        p2, pg2 = _mk_pg(ctx, "pg-c"); probes.append(p2); pgs.append(pg2)
        rep2 = pg2.join(GROUP); assert int(rep2.get("status", 1)) == 0
        # baseline counts (member 0 already has 1 from BASIC)
        base = {id(pg0): len(pg0.await_broadcast(GROUP, 0, 0)),
                id(pg1): len(pg1.await_broadcast(GROUP, 0, 0)),
                id(pg2): len(pg2.await_broadcast(GROUP, 0, 0))}
        pgs_send.broadcast(GROUP, GROUP_PKG, GROUP,
                           timestamp_ms=999, uptime_ms=888, generation=2, total_workers=3, active_workers=3)
        g0 = pg0.await_broadcast(GROUP, base[id(pg0)] + 1, 3.0)
        g1 = pg1.await_broadcast(GROUP, base[id(pg1)] + 1, 3.0)
        g2 = pg2.await_broadcast(GROUP, base[id(pg2)] + 1, 3.0)
        print(f"   counts after broadcast: a={len(g0)} b={len(g1)} c={len(g2)}")
        ok3 = (len(g0) > base[id(pg0)] and len(g1) > base[id(pg1)]
               and len(g2) > base[id(pg2)])
        assert ok3, "N-RECEIVE: not all 3 members received the single broadcast"
        print("   N-RECEIVE OK (all 3 received one multicast)")

        print("\nALL PG MULTICAST TESTS PASSED")
    except AssertionError as e:
        print("FAIL:", e); rc = 1
    except Exception as e:
        print("ERROR:", type(e).__name__, e); rc = 2
    finally:
        cleanup()
    return rc


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Verify Snapshot/RestoreSnapshot (config-prefix scoped) via artheia.probe.

Put values, Snapshot('s1'), mutate + add + delete-equivalent, RestoreSnapshot
('s1'), confirm the snapshot's values are back. Works on either backend; run
per as configured (etcd default or THEIA_PER_BACKEND=mem).
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import ArtheiaContext  # noqa: E402

ART = REPO / "system/services/per/package.art"
PROTO = REPO / "platform/proto"


def main() -> int:
    ctx = ArtheiaContext(str(ART), proto_root=str(PROTO))
    probe = ctx.probe("PerClient").start()

    def call(node, op, **kw):
        last = None
        for _ in range(20):
            try:
                return probe.call(node, op, timeout=4.0, **kw)
            except (TimeoutError, ConnectionError, OSError) as e:
                last = e
                probe.reset_clients()
        raise last

    ok = True
    try:
        print("== Put baseline: a=1, b=2 @ v1 ==")
        call("PerClient", "PutConfig", target_node="snap_a", config=b"one",
             digest="v1", expect_rev=0)
        call("PerClient", "PutConfig", target_node="snap_b", config=b"two",
             digest="v1", expect_rev=0)

        print("== Snapshot('s1') ==")
        r = call("PerManager", "Snapshot", label="s1")
        print("  ", r); ok &= r.get("status") == 0 and "2 keys" in r.get("message", "")

        print("== mutate after snapshot: a->MUTATED, add c ==")
        call("PerClient", "PutConfig", target_node="snap_a", config=b"MUTATED",
             digest="v2", expect_rev=0)
        call("PerClient", "PutConfig", target_node="snap_c", config=b"three",
             digest="v1", expect_rev=0)

        print("== RestoreSnapshot('s1') ==")
        r = call("PerManager", "RestoreSnapshot", label="s1")
        print("  ", r); ok &= r.get("status") == 0 and "restored 2" in r.get("message", "")

        print("== verify: a/b back to snapshot values ==")
        a = call("PerClient", "GetConfig", target_node="snap_a", want_digest="")
        b = call("PerClient", "GetConfig", target_node="snap_b", want_digest="")
        print("  snap_a:", a.get("config"), a.get("digest"))
        print("  snap_b:", b.get("config"), b.get("digest"))
        ok &= a.get("config") == b"one" and a.get("digest") == "v1"   # restored
        ok &= b.get("config") == b"two"
        # snap_c was added AFTER the snapshot — restore re-puts s1's keys but
        # doesn't delete extras, so c survives (restore = overlay, not wipe).
        c = call("PerClient", "GetConfig", target_node="snap_c", want_digest="")
        print("  snap_c (post-snapshot add, survives overlay restore):", c.get("config"))

        print("== RestoreSnapshot of a missing label -> error ==")
        r = call("PerManager", "RestoreSnapshot", label="nope")
        print("  ", r); ok &= r.get("status") != 0

        print("RESULT:", "OK" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        probe.stop()


if __name__ == "__main__":
    raise SystemExit(main())

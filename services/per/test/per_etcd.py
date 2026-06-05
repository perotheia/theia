#!/usr/bin/env python3
"""End-to-end test of per against REAL etcd via artheia.probe.

Put/Get round-trip, CAS using etcd's actual (global) revisions, and that the
value lands in etcd. Run `per` WITHOUT THEIA_PER_BACKEND (defaults to etcd).
Needs etcd reachable at 127.0.0.1:2379; the per fixture cleans /theia/config/.
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
    probe = ctx.probe("PerManager").start()
    ok = True
    try:
        NODE = "etcd_test_node"

        print("== Put #1 (unconditional, expect_rev=0) ==")
        r = probe.call("PerClient", "PutConfig", target_node=NODE,
                       config=b"alpha", digest="v1", expect_rev=0)
        print("  ", r); ok &= r.get("status") == 0
        rev1 = r.get("mod_rev")

        print("== Get -> alpha/v1 ==")
        r = probe.call("PerClient", "GetConfig", target_node=NODE, want_digest="")
        print("  ", {k: r.get(k) for k in ("config", "digest", "mod_rev")})
        ok &= r.get("config") == b"alpha" and r.get("digest") == "v1"
        ok &= r.get("mod_rev") == rev1   # get's mod_rev == put's revision

        print("== Put #2 CAS with correct rev -> succeeds ==")
        r = probe.call("PerClient", "PutConfig", target_node=NODE,
                       config=b"beta", digest="v2", expect_rev=rev1)
        print("  ", r); ok &= r.get("status") == 0
        rev2 = r.get("mod_rev")
        ok &= rev2 > rev1

        print("== Put #3 CAS with STALE rev -> conflict ==")
        r = probe.call("PerClient", "PutConfig", target_node=NODE,
                       config=b"gamma", digest="v3", expect_rev=rev1)
        print("  ", r); ok &= r.get("status") == 1

        print("== Get -> still beta/v2 (gamma rejected) ==")
        r = probe.call("PerClient", "GetConfig", target_node=NODE, want_digest="")
        ok &= r.get("config") == b"beta" and r.get("digest") == "v2"
        print("  config:", r.get("config"))

        print("RESULT:", "OK" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        probe.stop()


if __name__ == "__main__":
    raise SystemExit(main())

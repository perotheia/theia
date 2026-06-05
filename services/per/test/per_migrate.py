#!/usr/bin/env python3
"""Verify lazy migration-on-read (the transform chain) via artheia.probe.

The example migrations register v1->v2 (append "+v2") and v2->v3 (append "+v3").
Put a value at digest v1, then GetConfig with want_digest=v3 -> per walks the
chain (v1->v2->v3) and returns the reshaped bytes tagged v3. Run per with
THEIA_PER_BACKEND=mem (no etcd revs needed).
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
        NODE = "mig_node"

        print("== Put alpha @ v1 ==")
        probe.call("PerClient", "PutConfig", target_node=NODE,
                   config=b"alpha", digest="v1", expect_rev=0)

        print("== Get want=v1 (stored) -> alpha / v1 (no migration) ==")
        r = probe.call("PerClient", "GetConfig", target_node=NODE, want_digest="v1")
        print("  ", r.get("config"), r.get("digest"))
        ok &= r.get("config") == b"alpha" and r.get("digest") == "v1"

        print("== Get want=v2 -> alpha+v2 / v2 (one hop) ==")
        r = probe.call("PerClient", "GetConfig", target_node=NODE, want_digest="v2")
        print("  ", r.get("config"), r.get("digest"))
        ok &= r.get("config") == b"alpha+v2" and r.get("digest") == "v2"

        print("== Get want=v3 -> alpha+v2+v3 / v3 (chained v1->v2->v3) ==")
        r = probe.call("PerClient", "GetConfig", target_node=NODE, want_digest="v3")
        print("  ", r.get("config"), r.get("digest"))
        ok &= r.get("config") == b"alpha+v2+v3" and r.get("digest") == "v3"

        print("== Get want=v9 (no path) -> stored verbatim alpha / v1 ==")
        r = probe.call("PerClient", "GetConfig", target_node=NODE, want_digest="v9")
        print("  ", r.get("config"), r.get("digest"))
        ok &= r.get("config") == b"alpha" and r.get("digest") == "v1"

        print("RESULT:", "OK" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        probe.stop()


if __name__ == "__main__":
    raise SystemExit(main())

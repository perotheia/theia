#!/usr/bin/env python3
"""Verify MigrateBulk: dlopen the n+1 transform plugin + bulk-rewrite the store.

Puts several values at digest v1, then calls MigrateBulk(v1->v3, plugin_so=the
example .so). per dlopen's the plugin (registering v1->v2, v2->v3), walks the
keyspace, and rewrites every v1 value to v3 (chained: alpha -> alpha+v2+v3).
Also proves the SAME loaded plugin now serves lazy migration-on-read.

Run per with THEIA_PER_BACKEND=mem (simplest; bulk works on any store).
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import ArtheiaContext  # noqa: E402

ART = REPO / "system/services/per/package.art"
PROTO = REPO / "platform/proto"
PLUGIN = REPO / "bazel-bin/services/per/migrations/libper_migrate_example.so"


def main() -> int:
    if not PLUGIN.exists():
        print("plugin .so not built:", PLUGIN)
        return 2
    ctx = ArtheiaContext(str(ART), proto_root=str(PROTO))
    probe = ctx.probe("PerClient").start()

    def call(node, op, **kw):
        # Robust against stale TIPC co-bindings (leftover per instances): the
        # name-addressed connect load-balances across dead+live ports; retry on
        # a fresh connection until one lands on the live binding.
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
        nodes = {"n_a": b"alpha", "n_b": b"beta", "n_c": b"gamma"}
        print("== Put 3 values @ v1 ==")
        for n, v in nodes.items():
            call("PerClient", "PutConfig", target_node=n,
                       config=v, digest="v1", expect_rev=0)
        # one value already at a different digest — must be skipped
        call("PerClient", "PutConfig", target_node="n_other",
                   config=b"x", digest="v9", expect_rev=0)

        print("== MigrateBulk v1->v3 with the dlopen plugin ==")
        r = call("PerManager", "MigrateBulk", config_type="DemoCfg",
                       from_digest="v1", to_digest="v3", plugin_so=str(PLUGIN))
        print("  ", r)
        ok &= r.get("status") == 0
        # 3 migrated, 1 skipped (n_other @ v9)
        ok &= "migrated 3" in r.get("message", "") and "skipped 1" in r.get("message", "")

        print("== Get each -> chained alpha+v2+v3 @ v3 ==")
        for n, v in nodes.items():
            g = call("PerClient", "GetConfig", target_node=n, want_digest="")
            exp = v + b"+v2+v3"
            print(f"  {n}: {g.get('config')} digest={g.get('digest')} (want {exp})")
            ok &= g.get("config") == exp and g.get("digest") == "v3"

        print("== n_other untouched (still v9) ==")
        g = call("PerClient", "GetConfig", target_node="n_other", want_digest="")
        ok &= g.get("config") == b"x" and g.get("digest") == "v9"

        print("== lazy read uses the SAME plugin: Get n_other-equivalent at v1 then want v2 ==")
        call("PerClient", "PutConfig", target_node="n_lazy",
                   config=b"zeta", digest="v1", expect_rev=0)
        g = call("PerClient", "GetConfig", target_node="n_lazy", want_digest="v2")
        print("  n_lazy want=v2 ->", g.get("config"), g.get("digest"))
        ok &= g.get("config") == b"zeta+v2" and g.get("digest") == "v2"

        print("RESULT:", "OK" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        probe.stop()


if __name__ == "__main__":
    raise SystemExit(main())

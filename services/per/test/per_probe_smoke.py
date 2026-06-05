#!/usr/bin/env python3
"""Smoke-drive the live per FC via artheia.probe.

Impersonates PerManager (a node in per's own .art) and calls PerClient's
GetConfig / PutConfig over real TIPC. With stub handlers this just proves the
wire path (encode .art req -> FC handle_call -> decode reply); once handlers
land it round-trips a real config. Run with `per` already listening.
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
    # Impersonate PerManager (has its own address) to drive PerClient's ops.
    probe = ctx.probe("PerManager").start()
    try:
        print("== GetConfig(target=supervisor_ctl) ==")
        rep = probe.call("PerClient", "GetConfig",
                         target_node="supervisor_ctl", want_digest="")
        print("  reply:", rep)

        print("== PutConfig(target=supervisor_ctl, 12 bytes) ==")
        rep = probe.call("PerClient", "PutConfig",
                         target_node="supervisor_ctl",
                         config=b"hello-config",
                         digest="v1", expect_rev=0)
        print("  reply:", rep)

        print("== GetConfig AGAIN (should now return the stored value) ==")
        rep = probe.call("PerClient", "GetConfig",
                         target_node="supervisor_ctl", want_digest="")
        print("  reply:", rep)
        ok = (rep.get("config") == b"hello-config" and rep.get("digest") == "v1"
              and rep.get("mod_rev") == 1)
        print("  round-trip:", "OK" if ok else "MISMATCH")

        print("== WatchConfig(target=supervisor_ctl, sub=counter) ==")
        rep = probe.call("PerClient", "WatchConfig",
                         target_node="supervisor_ctl",
                         subscriber_node="counter", want_digest="")
        print("  reply:", rep)

        print("== PutConfig again (rev should bump to 2, CAS expect_rev=1) ==")
        rep = probe.call("PerClient", "PutConfig",
                         target_node="supervisor_ctl",
                         config=b"v2-config", digest="v2", expect_rev=1)
        print("  reply:", rep)

        print("== PutConfig with stale CAS expect_rev=1 (should conflict) ==")
        rep = probe.call("PerClient", "PutConfig",
                         target_node="supervisor_ctl",
                         config=b"v3", digest="v3", expect_rev=1)
        print("  reply:", rep, "(expect status=1 CAS conflict)")
        return 0 if ok else 1
    finally:
        probe.stop()


if __name__ == "__main__":
    raise SystemExit(main())

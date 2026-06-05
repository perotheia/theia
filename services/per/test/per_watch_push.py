#!/usr/bin/env python3
"""Verify the ConfigUpdated subscription PUSH end-to-end via artheia.probe.

This exercises the receiver-side probe mocking (await_cast) added for the per
config gatekeeper:

  1. A probe impersonating PerManager (a real per .art node, addr 0x80010008)
     SUBSCRIBES to supervisor_ctl's config (WatchConfig).
  2. The same probe does a PutConfig.
  3. per resolves the subscriber (PerManager) via netgraph.json and casts
     platform.runtime.ConfigUpdated back to the probe.
  4. The probe await_cast("ConfigUpdated") catches the push and asserts the
     payload matches what was Put.

Run `per` with THEIA_NETGRAPH=services/per/test/per_netgraph.json so per can
resolve PerManager's address.
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
    # Bind an EXTERNAL subscriber address (0x800100ff) that per does NOT run —
    # so per's ConfigUpdated cast lands on THIS probe, not per's own PerManager.
    # "test_sub" maps to 0x800100ff in per_netgraph.json so per can resolve it.
    SUB_TYPE = 0x800100FF
    probe = ctx.probe_external(SUB_TYPE, 0, name="test_sub").start()
    try:
        # Pre-arm the inbox so the async push (which may land before await_cast)
        # is queued, not dropped.
        probe.arm_cast("ConfigUpdated")

        print("== WatchConfig(target=supervisor_ctl, subscriber=test_sub) ==")
        rep = probe.call("PerClient", "WatchConfig",
                         target_node="supervisor_ctl",
                         subscriber_node="test_sub", want_digest="")
        print("  watch reply:", rep)

        print("== PutConfig(target=supervisor_ctl) -> should push ConfigUpdated ==")
        rep = probe.call("PerClient", "PutConfig",
                         target_node="supervisor_ctl",
                         config=b"pushed-payload", digest="v7", expect_rev=0)
        print("  put reply:", rep)

        print("== await ConfigUpdated cast on the subscriber ==")
        got = probe.await_cast("ConfigUpdated", timeout=3.0)
        print("  got cast:", {k: got.get(k) for k in ("config", "digest", "changed")})
        ok = (got.get("config") == b"pushed-payload"
              and got.get("digest") == "v7")
        print("  PUSH:", "OK" if ok else "MISMATCH")
        return 0 if ok else 1
    finally:
        probe.stop()


if __name__ == "__main__":
    raise SystemExit(main())

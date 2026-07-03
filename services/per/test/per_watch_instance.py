#!/usr/bin/env python3
"""Verify per's PER-INSTANCE config notify: a change to a <component>/<instance>
key notifies ONLY the node clone at that instance.

The model: N clones of one node share code + TIPC type, differ only by instance
(round-robin). Each clone stores its OWN config in per under <component>/<instance>
(//counter/0, //counter/1, …). per is notified per key, parses the instance from
the key, and casts ConfigUpdated to (subscriber_type, that_instance) — the exact
clone. Confinement falls out: each clone watches its own key.

This test stands in 3 subscriber clones (instances 0/1/2) at ONE TIPC type
(test_sub → 0x800100ff in per_netgraph.json), each watching its own counter/<i>
key. A PutConfig on counter/1 must land ONLY on the instance-1 probe.

Run `per` with THEIA_NETGRAPH=services/per/test/per_netgraph.json.
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import ArtheiaContext  # noqa: E402

ART = REPO / "system/services/per/package.art"
PROTO = REPO / "platform/proto"
SUB_TYPE = 0x800100FF          # test_sub's type in per_netgraph.json


def main() -> int:
    ctx = ArtheiaContext(str(ART), proto_root=str(PROTO))
    # Three clones of the SAME subscriber, distinct TIPC instances 0/1/2.
    probes = {i: ctx.probe_external(SUB_TYPE, i, name=f"test_sub").start()
              for i in (0, 1, 2)}
    try:
        for i, pr in probes.items():
            pr.arm_cast("ConfigUpdated")
            # Each clone watches its OWN per-instance key counter/<i>, declaring its
            # instance so per matches + notifies exactly it.
            pr.call("PerClient", "WatchConfig",
                    target_node=f"counter/{i}",
                    subscriber_node="test_sub", subscriber_instance=i,
                    want_digest="")
        print("== 3 clones armed + watching counter/0, counter/1, counter/2 ==")

        # Change ONLY counter/1 → only the instance-1 clone must be notified.
        print("== PutConfig(counter/1) — expect ONLY instance 1 notified ==")
        probes[1].call("PerClient", "PutConfig", target_node="counter/1",
                       config=b"cfg-for-clone-1", digest="v1", expect_rev=0)

        got1 = probes[1].await_cast("ConfigUpdated", timeout=3.0)
        ok1 = bool(got1) and got1.get("config") == b"cfg-for-clone-1"
        print(f"  instance 1 got its config: {'OK' if ok1 else 'MISS'}")

        # The other clones must NOT receive counter/1's change (confinement).
        # await_cast RAISES TimeoutError when nothing arrives — which is exactly
        # what we want here, so a timeout == confined (no leak).
        leaked = []
        for i in (0, 2):
            try:
                g = probes[i].await_cast("ConfigUpdated", timeout=0.8)
                if g:
                    leaked.append(i)
            except TimeoutError:
                pass   # no cast → correctly NOT notified
        ok_confined = not leaked
        print(f"  instances 0,2 NOT notified: "
              f"{'OK' if ok_confined else f'LEAKED to {leaked}'}")

        ok = ok1 and ok_confined
        print("PER-INSTANCE NOTIFY:", "OK" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        for pr in probes.values():
            pr.stop()


if __name__ == "__main__":
    raise SystemExit(main())

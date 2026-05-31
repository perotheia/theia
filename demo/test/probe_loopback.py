#!/usr/bin/env python3
"""End-to-end exercise of artheia.gen_server.probe over the real TIPC wire.

The probe is fully generic — it learns everything from a parsed .art. This
script just feeds it the demo spec and drives two probes against each other to
prove every gen_server operation on the actual AF_TIPC/SOCK_SEQPACKET wire:

  - cast  (active)  -> on_cast / expect_cast (passive)   [MSG_GEN_CAST]
  - call  (active)  -> on_call + reply        (passive)   [MSG_GEN_CALL/_REPLY]

No C++, no demo logic in the probe: one probe impersonates CounterNode
(answers Get, accumulates Inc), another impersonates DriverNode (casts Inc,
calls Get). Same wire a real CounterNode FC would speak.

Run:
    PATH="$PWD/.venv/bin:$PATH" python demo/test/probe_loopback.py
(needs the TIPC kernel module: `sudo modprobe tipc`)
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import ArtheiaContext  # noqa: E402


def main() -> int:
    ctx = ArtheiaContext(
        str(REPO / "demo/system/demo/component.art"),
        proto_root=str(REPO / "platform/proto"),
    )
    print(f"[ctx] parsed {ctx.package}; nodes: {', '.join(sorted(vars(ctx.nodes)))}")

    # --- passive probe: mock CounterNode (the 'FC under test' stand-in) ---
    counter = ctx.probe("CounterNode")
    state = {"value": 0}
    counter.on_cast("Inc", lambda f: state.__setitem__("value", state["value"] + f["n"]))
    counter.on_call("Get", lambda req: {"value": state["value"]})
    counter.start()
    print(f"[counter] up @ 0x{counter.me.tipc_type:08x} — answers Get, accumulates Inc")

    # --- active probe: drive it like the real DriverNode would ---
    driver = ctx.probe("DriverNode").start()
    print(f"[driver]  up @ 0x{driver.me.tipc_type:08x}")

    # cast 10x Inc{n=5}  (MSG_GEN_CAST, fire-and-forget)
    for _ in range(10):
        driver.cast("CounterNode", "Inc", n=5)
    print("[driver]  sent 10x cast(Inc{n=5})")

    # call Get  (MSG_GEN_CALL -> MSG_GEN_CALL_REPLY)
    reply = driver.call("CounterNode", "Get", timeout=2.0)
    print(f"[driver]  call(Get) -> {reply}")

    ok = reply.get("value") == 50
    print(f"[check]   value=={reply.get('value')} expected=50 -> {'PASS' if ok else 'FAIL'}")

    # The on_cast handler observed all 10 increments (the passive cast path).
    inc_ok = state["value"] == 50
    print(f"[check]   on_cast accumulated value={state['value']} -> "
          f"{'PASS' if inc_ok else 'FAIL'}")

    driver.stop()
    counter.stop()
    return 0 if (ok and inc_ok) else 1


if __name__ == "__main__":
    sys.exit(main())

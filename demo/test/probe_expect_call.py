#!/usr/bin/env python3
"""Exercise NodeProbe.expect_call — the passive call-assertion path.

A probe impersonating CounterNode (which `provides CounterSrv`) uses
expect_call("Get", reply=...) to BLOCK until a call lands, assert on the
request, and reply inline. A second probe impersonating DriverNode (a
`client` of CounterSrv) issues the call. Proves MSG_GEN_CALL -> probe captures
request -> probe replies MSG_GEN_CALL_REPLY -> caller decodes it, all over the
real TIPC wire, with no on_call responder registered.

Run:
    PATH="$PWD/.venv/bin:$PATH" python demo/test/probe_expect_call.py
"""
import sys
import threading
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import ArtheiaContext  # noqa: E402


def main() -> int:
    ctx = ArtheiaContext(
        str(REPO / "demo/system/demo/component.art"),
        proto_root=str(REPO / "platform/proto"),
    )

    server = ctx.probe("CounterNode").start()   # binds CounterSrv address
    client = ctx.probe("DriverNode").start()
    print(f"[server] CounterNode up @ 0x{server.me.tipc_type:08x}")
    print(f"[client] DriverNode  up @ 0x{client.me.tipc_type:08x}")

    # The caller runs on a thread (call() blocks for the reply); the main
    # thread does expect_call() which captures the request and replies 99.
    result = {}

    def do_call():
        result["reply"] = client.call("CounterNode", "Get", timeout=3.0)

    t = threading.Thread(target=do_call)
    t.start()

    # Passive assertion: block until the Get call arrives, reply value=99,
    # and get the request back to assert on.
    request = server.expect_call("Get", reply={"value": 99}, timeout=3.0)
    print(f"[server] expect_call(Get) captured request: {request}")

    t.join(timeout=4.0)
    reply = result.get("reply")
    print(f"[client] call(Get) -> {reply}")

    ok = reply is not None and reply.get("value") == 99
    print(f"[check]  reply value=={reply.get('value') if reply else None} "
          f"expected=99 -> {'PASS' if ok else 'FAIL'}")

    # Get is paramless, so the captured request is an empty message ({}).
    req_ok = request == {}
    print(f"[check]  captured request {request} is empty (Get is paramless) -> "
          f"{'PASS' if req_ok else 'FAIL'}")

    client.stop()
    server.stop()
    return 0 if (ok and req_ok) else 1


if __name__ == "__main__":
    sys.exit(main())

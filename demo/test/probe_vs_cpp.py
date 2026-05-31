#!/usr/bin/env python3
"""Prove the Python probe interops with a REAL C++ FC over TIPC.

Launches the actual Demo3WayP1 binary (CounterNode + DriverNode + Ticker in one
process). The probe — impersonating a new client — does its OWN cast(Inc) and
call(Get) against the real CounterNode's C++ TipcMux, exercising the exact
service_id / TheiaMsgHeader / proto3 wire the C++ register_call/register_cast
expect. Success = the C++ node decodes the probe's frames and replies.

The bundled C++ DriverNode also casts 10x Inc{5} (-> 50), so the probe reads a
real value and then adds its own increments on top: it asserts the reply is a
valid GetReply and that its own casts move the counter, i.e. the C++ side
accepted and dispatched the probe's frames.

Run (after `bazel build //demo/Demo3WayP1/main:demo`):
    PATH="$PWD/.venv/bin:$PATH" python demo/test/probe_vs_cpp.py
"""
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import ArtheiaContext  # noqa: E402

BINARY = REPO / "bazel-bin/demo/Demo3WayP1/main/demo"


def _call_retry(probe, op, attempts=4, timeout=3.0, **fields):
    """call() with retry — a fresh SEQPACKET client per try (the bundled C++
    driver's reconnect burst can win the first race; a clean client wins)."""
    last = None
    for i in range(attempts):
        try:
            return probe.call("CounterNode", op, timeout=timeout, **fields)
        except TimeoutError as e:
            last = e
            probe.reset_clients()  # force a brand-new connection next try
    raise last


def main() -> int:
    if not BINARY.exists():
        print(f"missing {BINARY} — run: bazel build --config=linux "
              f"//demo/Demo3WayP1/main:demo", file=sys.stderr)
        return 2

    ctx = ArtheiaContext(
        str(REPO / "demo/system/demo/component.art"),
        proto_root=str(REPO / "platform/proto"),
    )

    # Launch the real C++ FC process (CounterNode binds 0xd0010001).
    proc = subprocess.Popen(
        [str(BINARY)], env={"THEIA_LOG_LEVEL": "info"},
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        # Let CounterNode bind AND the bundled C++ DriverNode finish its burst
        # (it hammers the listener with reconnect attempts while active, which
        # races a fresh probe connection — wait for it to settle).
        time.sleep(2.0)

        # Probe impersonates ObserverNode (a real client of CounterNode.srv,
        # distinct TIPC addr 0xd0010004) and drives the live C++ CounterNode.
        probe = ctx.probe("ObserverNode").start()

        # 1) call Get on the real C++ CounterNode -> proves call/reply interop.
        #    Retry once: a SEQPACKET connect can lose the first race with the
        #    bundled driver's reconnects; a fresh client wins.
        before = _call_retry(probe, "Get")
        print(f"[probe] call(Get) on C++ CounterNode -> {before}")
        if "value" not in before:
            print("[FAIL] no value field in reply — wire/codec mismatch")
            return 1
        base = before["value"]

        # 2) the probe casts its own Inc{n=7}; the C++ register_cast<Inc> must
        #    decode + dispatch it. Re-read to confirm the counter moved.
        #    (ObserverNode has no inc_out port; cast via the IncIface message
        #    directly using DriverNode's view, which the C++ side accepts by
        #    service_id regardless of which peer sent it.)
        driver = ctx.probe("DriverNode").start()
        driver.cast("CounterNode", "Inc", n=7)
        time.sleep(0.3)
        after = _call_retry(probe, "Get")
        print(f"[probe] after cast(Inc{{n=7}}): {after}")

        moved = after["value"] == base + 7
        print(f"[check] C++ counter {base} -> {after['value']} (+7) -> "
              f"{'PASS' if moved else 'FAIL'}")

        probe.stop()
        driver.stop()
        return 0 if moved else 1
    finally:
        proc.terminate()
        proc.wait(timeout=3)


if __name__ == "__main__":
    sys.exit(main())

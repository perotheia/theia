#!/usr/bin/env python3
"""Drive the DemoFsm gen_statem FC standalone via artheia.probe.

Builds a probe from demo_fsm_tester.art (the DemoFsmTester sender node), then
casts DemoStart / DemoFinish / DemoReset at DemoFsmGate (tipc 0xd0010007) over
ONE persistent, ORDERED connection — so the FSM sees IDLE→PROCESSING→DONE→IDLE
in order (unlike raw per-event sockets, which race). The gate post_event()s
each into the in-process FSM.

This is the event-injection half of the statem rf-testing loop (Step B). The
observer/`Wait For State` half (Step C) asserts the resulting STATEM trace.

Run (supervisor + p4 live, e.g. `theia install central` then the supervisor):
    PATH="$PWD/.venv/bin:$PATH" python demo/test/fsm_drive.py
"""
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import ArtheiaContext   # noqa: E402

# Canonical system/ path so `import system.demo.*` resolves (the dir-climb
# mirrors the package FQN). Parsing the physical path would fail the import.
ART = REPO / "system/demo_test/demo_fsm_tester.art"
PROTO = REPO / "platform/proto"

# The DemoFsmIn events, in the order that walks IDLE→PROCESSING→DONE→IDLE.
SEQUENCE = ["DemoStart", "DemoFinish", "DemoReset"]


def main() -> int:
    ctx = ArtheiaContext(str(ART), proto_root=str(PROTO))
    # Bind DemoFsmTester as the cast SOURCE; one probe = one ordered connection.
    probe = ctx.probe("DemoFsmTester").start()
    try:
        gate = ctx.ref("DemoFsmGate")
        print(f"[drive] gate DemoFsmGate @ tipc 0x{gate.tipc_type:08x}")
        for ev in SEQUENCE:
            probe.cast("DemoFsmGate", ev)
            print(f"[drive] cast {ev} → DemoFsmGate")
            time.sleep(0.4)   # let each transition + on_enter + trace settle
        print(f"[drive] sent {len(SEQUENCE)} events in order")
        return 0
    finally:
        probe.stop()


if __name__ == "__main__":
    sys.exit(main())

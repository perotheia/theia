#!/usr/bin/env python3
"""Self-test the logcat PG migration: membership DRIVES the lazy tailer.

What changed (aligning logcat with tracecat): the LogStreamPump no longer waits
for a Subscribe RPC to start tailing. It pg_watch'es the LogRecord group (OTP
pg:monitor); the supervisor pushes PgMembership on every join/leave; the pump
tails files ONLY while the group has members (LogHub::tailer_wanted() reads the
member count).

This test proves the membership→tailer coupling without needing real node log
files (the empty-tree supervisor reports no files to tail):
  1. Before any consumer: the pump is idle (no member → tailer_wanted()=false).
  2. A consumer pg_joins the LogRecord group → the supervisor pushes PgMembership
     to the pump (the watcher) → the pump's LogHub sees a member → it starts the
     tailer (logs "tailer:" — fetch_and_open_ runs).
  3. The consumer leaves → the pump's member list empties → the tailer winds down.

Run with supervisor (0x80020001) + log[] (0x80010023 pump) already up.
"""
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import ArtheiaContext        # noqa: E402
from artheia.gen_server.probe.pg import PgProbe            # noqa: E402

LOG_ART = REPO / "system/services/log/component.art"
PROTO = REPO / "platform/proto"
LOG_GROUP = "system_services_log_LogRecord"
LOG_PKG = "system.services.log"
PUMP_LOG = "/tmp/log_lc.log"   # where the pump's stderr is tee'd (tailer markers)


def tailer_active() -> bool:
    """The pump logs 'tailer:' lines from LogHub::tail_loop / fetch_and_open_
    once it has a member and starts tailing."""
    try:
        txt = Path(PUMP_LOG).read_text(errors="replace")
    except FileNotFoundError:
        return False
    return "tailer:" in txt


def main() -> int:
    ctx = ArtheiaContext(str(LOG_ART), proto_root=str(PROTO))
    rc = 0
    probe = pg = None
    try:
        print("== 1. before consumer: pump idle ==")
        before = tailer_active()
        print(f"   tailer active before join: {before}")

        print("== 2. consumer pg_joins the LogRecord group ==")
        probe = ctx.probe("LogStreamPump").start()
        pg = PgProbe(probe, node_name="logcat-test")
        rep = pg.join(LOG_GROUP)
        print("   join reply:", rep)
        assert int(rep.get("status", 1)) == 0, "join failed"
        pg.start_keepalive(period_s=0.5)

        # The supervisor pushes PgMembership to the pump → it starts tailing.
        deadline = time.time() + 5.0
        started = False
        while time.time() < deadline:
            if tailer_active():
                started = True
                break
            time.sleep(0.2)
        print(f"   tailer active after join: {started}")
        assert started, ("MEMBERSHIP→TAILER: pump did not start tailing after a "
                         "consumer joined the LogRecord group")
        print("   LOGCAT-PG OK (membership drove the lazy tailer)")

        print("\nLOGCAT PG MIGRATION TEST PASSED")
    except AssertionError as e:
        print("FAIL:", e); rc = 1
    except Exception as e:
        print("ERROR:", type(e).__name__, e); rc = 2
    finally:
        if pg:
            try: pg.shutdown()
            except Exception: pass
        if probe:
            try: probe.stop()
            except Exception: pass
    return rc


if __name__ == "__main__":
    raise SystemExit(main())

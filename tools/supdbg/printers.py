# supdbg.printers — text/JSON rendering for protos.
#
# Kept separate from client.py so the lib is import-cheap and so
# tests can poke .raw protos directly without depending on the
# print format.

from __future__ import annotations

import json
import sys
import typing as t

from google.protobuf.json_format import MessageToDict

from .client import Observation, EventKind

# -- field name helpers ----------------------------------------------

_EVENT_NAMES = {k.value: k.name for k in EventKind}

# ChildState.state enum from platform/supervisor/system/package.art
# (mirrored here so we don't drag the enum out of the proto module).
_STATE_NAMES = {
    0: "UNKNOWN",
    1: "STARTING",
    2: "RUNNING",
    3: "EXITED",
    4: "TERMINATED",
    5: "RESTARTING",
}


def _state_name(s: int) -> str:
    return _STATE_NAMES.get(s, f"?{s}")


def _event_name(k: int) -> str:
    return _EVENT_NAMES.get(k, f"?{k}")


# -- snapshot --------------------------------------------------------

def print_snapshot(snap, out: t.IO[str]) -> None:
    """Render TreeSnapshot as a flat table, parent column first so the
    eye can group children by supervisor:

        gen=N  ts=12345ms  children=18
        NAME              PARENT       STATE     PID    UPTIME    REST   CPU%  RSS-MB
        demo_p1           app_sup      RUNNING   1234   00:01:23     0   0.5    8
        ...
    """
    children = list(snap.children)
    print(f"gen={snap.generation}  ts={snap.timestamp_ms}ms  "
          f"children={len(children)}", file=out)
    if not children:
        return
    hdr = f"{'NAME':<22} {'PARENT':<18} {'STATE':<11} {'PID':>6} " \
          f"{'UPTIME':>10} {'REST':>5} {'CPU%':>5} {'RSS-MB':>7}"
    print(hdr, file=out)
    print("-" * len(hdr), file=out)
    for c in sorted(children, key=lambda c: (c.parent_name, c.name)):
        uptime = _fmt_uptime(c.uptime_ms)
        rss_mb = c.rss_kb // 1024 if c.rss_kb else 0
        print(
            f"{c.name:<22} {c.parent_name:<18} {_state_name(c.state):<11} "
            f"{c.pid:>6} {uptime:>10} {c.restart_count:>5} "
            f"{c.cpu_pct:>5} {rss_mb:>7}",
            file=out,
        )


def snapshot_json(snap) -> str:
    return json.dumps(MessageToDict(snap, preserving_proto_field_name=True),
                      indent=2)


def _fmt_uptime(ms: int) -> str:
    if ms <= 0:
        return "-"
    s = ms // 1000
    h, s = divmod(s, 3600)
    m, s = divmod(s, 60)
    if h:
        return f"{h}:{m:02d}:{s:02d}"
    return f"{m:02d}:{s:02d}"


# -- events ----------------------------------------------------------

def print_event(ev, out: t.IO[str]) -> None:
    """Compact single-line event format:

      ts=12345 EXITED demo_p1 (parent=app_sup) pid=1234 exit=127 detail=...
    """
    parts = [
        f"ts={ev.timestamp_ms}",
        _event_name(ev.kind),
        ev.child_name,
        f"(parent={ev.supervisor_name})" if ev.supervisor_name else "",
    ]
    if ev.pid:        parts.append(f"pid={ev.pid}")
    if ev.exit_code:  parts.append(f"exit={ev.exit_code}")
    if ev.strategy:   parts.append(f"strategy={ev.strategy}")
    if ev.detail:     parts.append(f"detail='{ev.detail}'")
    if ev.tombstone_path:
        parts.append(f"tombstone={ev.tombstone_path}")
    print(" ".join(s for s in parts if s), file=out)


# -- health beacon ---------------------------------------------------

def print_health(hb, out: t.IO[str]) -> None:
    print(
        f"ts={hb.timestamp_ms} HEALTH gen={hb.generation} "
        f"uptime={_fmt_uptime(hb.uptime_ms)} "
        f"workers={hb.active_workers}/{hb.total_workers} "
        f"restarts={hb.total_restarts} "
        f"tombstones={hb.total_tombstones}",
        file=out,
    )


# -- observation discriminator --------------------------------------

def print_observation(obs: Observation, out: t.IO[str]) -> None:
    """Single dispatch point used by both `watch` and the REPL's
    background tail."""
    prefix = f"[{obs.machine}] "
    if obs.event is not None:
        out.write(prefix); print_event(obs.event, out)
    elif obs.health is not None:
        out.write(prefix); print_health(obs.health, out)
    elif obs.snapshot is not None:
        out.write(prefix + f"SNAPSHOT gen={obs.snapshot.generation} "
                          f"children={len(obs.snapshot.children)}\n")
    else:
        out.write(prefix + "EMPTY\n")
    out.flush()


# -- ControlReply (returns shell-style exit code) -------------------

def print_reply(reply, out: t.IO[str]) -> int:
    """Print a single-line summary; return 0 if status==0 else 1 so the
    one-shot CLI propagates failure to the shell."""
    ok = (reply.status == 0)
    tag = "OK" if ok else f"ERR({reply.status})"
    pieces = [tag, reply.child_name or "(no-child)"]
    if reply.message:        pieces.append(f"msg='{reply.message}'")
    if reply.correlation_id: pieces.append(f"corr={reply.correlation_id}")
    print(" ".join(pieces), file=out)
    return 0 if ok else 1

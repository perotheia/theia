#!/usr/bin/env python3
# Installed on PATH as `tdb` via a .venv/bin/tdb symlink that `source env.sh`
# creates (alongside `theia`), mirroring how `.venv/bin/theia` → theia.py works.
"""tdb — the Theia Debug Bridge (adb for Theia).

Two modes, like adb:
  1. One-shot command from argv:    tdb ps
                                     tdb trace sm SmStateMsg
                                     tdb supervisor
  2. Interactive prompt_toolkit REPL (no command):  tdb

All transport is probe-backed (tools/tdb/system/tdb.art via artheia.probe) —
no raw TIPC. See feedback-clients-via-art-probe.

adb-shaped verbs:
  ps                       list nodes from the supervisor tree
  supervisor               supervisor host facts (GetSystemInfo)
  trace [off] <node> [mt]  ConfigureTrace node on/off (msgtype "" = all kinds)
  trace-config             show the stored trace config (GetTraceConfig)
  logcat [--json|-c|-g]    follow the trace firehose (subscribe to log[trace]);
                           --json = NDJSON (header + decoded inner proto) per line
  restart <name>           RestartChild
  terminate <name>         TerminateChild (stop-and-hold: no_restart=true)
  help / quit
"""
from __future__ import annotations

import json
import shlex
import sys
from pathlib import Path

# tdb_client lives next to this file.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from tdb_client import SupervisorClient, TraceClient  # noqa: E402

REPO = Path(__file__).resolve().parents[2]


# ---------------------------------------------------------------------------
# rendering
# ---------------------------------------------------------------------------

_STATE = {0: "stopped", 2: "running", 3: "terminating"}


def _g(obj, field, default=None):
    """Read a field from either a dict OR a protobuf message — the probe codec
    returns a shallow dict whose repeated-message values are protobuf objects."""
    if isinstance(obj, dict):
        return obj.get(field, default)
    return getattr(obj, field, default)


def _render_tree(rows) -> str:
    """rows = flat [{name,parent_name,kind,pid,state,...}] (dict or pb message);
    rebuild the hierarchy by parent_name + indent."""
    by_parent: dict[str, list] = {}
    for r in rows:
        by_parent.setdefault(_g(r, "parent_name", ""), []).append(r)
    out: list[str] = []

    # kind: 1=supervisor, 0=worker (process), 2=node (thread in the process).
    _KIND = {1: "sup", 0: "proc", 2: "node"}

    def walk(parent: str, depth: int) -> None:
        for r in by_parent.get(parent, []):
            kind = _KIND.get(_g(r, "kind"), "node")
            pid = _g(r, "pid", -1)
            st = _STATE.get(_g(r, "state", 0), str(_g(r, "state")))
            tag = _g(r, "strategy") or _g(r, "start_cmd") or ""
            pidstr = f"pid={pid}" if pid and pid > 0 else ""
            out.append(f"{'  ' * depth}{_g(r, 'name')} [{kind}] {st} {pidstr}"
                       f"{('  ' + tag) if tag else ''}".rstrip())
            walk(_g(r, "name"), depth + 1)

    walk("", 0)
    return "\n".join(out) if out else "(empty tree)"


# ---------------------------------------------------------------------------
# command handlers — each takes (args: list[str], sup, trace_factory)
# ---------------------------------------------------------------------------

def cmd_ps(args, sup, _tf) -> int:
    reply = sup.get_tree(timeout=3.0)
    rows = list(_g(reply, "children", []) or [])
    print(_render_tree(rows))
    return 0


def cmd_supervisor(args, sup, _tf) -> int:
    info = sup.get_system_info(timeout=3.0)
    for k in ("hostname", "kernel", "os_pretty_name", "cpu_count",
              "total_ram_kb", "uptime_sec"):
        print(f"{k:16} {_g(info, k, '')}")
    return 0


def cmd_trace(args, sup, _tf) -> int:
    # trace <node> [msg_type]        — enable
    # trace off <node> [msg_type]    — disable
    enabled = True
    if args and args[0] in ("off", "on"):
        enabled = args[0] == "on"
        args = args[1:]
    if not args:
        print("usage: trace [off] <node> [msg_type]", file=sys.stderr)
        return 2
    node = args[0]
    msg_type = args[1] if len(args) > 1 else ""
    rep = sup.configure_trace(target_node=node, msg_type=msg_type,
                              enabled=enabled, kind=0, timeout=3.0)
    verb = "on" if enabled else "off"
    status = _g(rep, "status")
    msg = _g(rep, "message", "")
    if status == 0:
        print(f"trace {verb}: {node} {msg_type or '(all)'} -> ok"
              f"{(' (' + msg + ')') if msg else ''}")
        return 0
    # Non-zero = the supervisor rejected it (e.g. unknown node name). Report
    # the supervisor's reason; don't pretend it worked.
    print(f"trace {verb}: {node} -> FAILED (status={status}): {msg or 'rejected'}",
          file=sys.stderr)
    return 1


def cmd_trace_config(args, sup, _tf) -> int:
    tc = sup.get_trace_config(timeout=3.0)
    cfgs = list(_g(tc, "configs", []) or [])
    if not cfgs:
        print("(no trace config)")
        return 0
    for c in cfgs:
        print(f"{_g(c, 'target_node')}  {_g(c, 'msg_type') or '(all)'}  "
              f"kind={_g(c, 'kind', 0)}")
    return 0


def cmd_restart(args, sup, _tf) -> int:
    if not args:
        print("usage: restart <name>", file=sys.stderr)
        return 2
    rep = sup.restart_child(args[0], timeout=3.0)
    print(f"restart {args[0]} -> status={_g(rep, 'status')}")
    return 0 if _g(rep, "status") == 0 else 1


def cmd_terminate(args, sup, _tf) -> int:
    if not args:
        print("usage: terminate <name>", file=sys.stderr)
        return 2
    rep = sup.terminate_hold(args[0], timeout=3.0)
    print(f"terminate (hold) {args[0]} -> status={_g(rep, 'status')}")
    return 0 if _g(rep, "status") == 0 else 1


def _fmt_content(content: dict) -> str:
    """Render a decoded inner message dict as `{k: v, ...}`. bytes → hex so a
    nested-payload field stays readable; everything else via repr-ish str."""
    def v(x):
        if isinstance(x, (bytes, bytearray)):
            return x.hex() if x else '""'
        return str(x)
    return "{" + ", ".join(f"{k}: {v(val)}" for k, val in content.items()) + "}"


def cmd_logcat(args, _sup, trace_factory) -> int:
    # -g get ring size / -c clear are firehose-control follow-ups; the default
    # is "follow live records" (subscribe to log[trace], decode + print).
    if "-g" in args or "-c" in args:
        print("logcat -g/-c (ring size / clear) — not wired yet", file=sys.stderr)
        return 2
    # --json: one JSON object per line (NDJSON) — full header + decoded inner
    # proto — for piping into jq / a log pipeline. No human banner on stdout.
    as_json = "--json" in args
    trace = trace_factory()
    if not as_json:
        print("logcat: following trace firehose (Ctrl-C to stop) ...")
    try:
        for rec in trace.records(timeout=600.0):
            if as_json:
                print(json.dumps(rec.to_dict(), separators=(",", ":")),
                      flush=True)
                continue
            # Prefer the DECODED inner message ({value: 860}) over the raw
            # base64 payload in the envelope JSON. content is None when the
            # record has no payload (e.g. Dispatch) or the type didn't resolve.
            body = _fmt_content(rec.content) if rec.content else ""
            print(f"{rec.ts_ns:>16} {rec.src:>16} {rec.msg_type:<22} "
                  f"corr={rec.corr_id}{(' ' + body) if body else ''}")
    except KeyboardInterrupt:
        pass
    finally:
        trace.stop()
    return 0


_COMMANDS = {
    "ps": cmd_ps,
    "supervisor": cmd_supervisor,
    "sup": cmd_supervisor,
    "trace": cmd_trace,
    "trace-config": cmd_trace_config,
    "restart": cmd_restart,
    "terminate": cmd_terminate,
    "logcat": cmd_logcat,
}

_HELP = """tdb — Theia Debug Bridge. commands:
  ps                       list the supervisor tree
  supervisor               supervisor host facts
  trace [off] <node> [mt]  turn tracing on/off for a node/worker
  trace-config             show stored trace config
  logcat [--json]          follow the trace firehose (--json = NDJSON)
  restart <name>           restart a child
  terminate <name>         stop-and-hold a child
  help                     this help
  quit / exit              leave the REPL"""


# ---------------------------------------------------------------------------
# lazy clients (connect on first use so `tdb help` needs no supervisor)
# ---------------------------------------------------------------------------

class _Session:
    def __init__(self) -> None:
        self._sup = None

    @property
    def sup(self) -> SupervisorClient:
        if self._sup is None:
            self._sup = SupervisorClient.from_workspace(REPO)
        return self._sup

    def trace_factory(self) -> TraceClient:
        return TraceClient.from_workspace(REPO)

    def close(self) -> None:
        if self._sup is not None:
            self._sup.stop()


def _dispatch(sess: _Session, verb: str, args: list[str]) -> int:
    fn = _COMMANDS.get(verb)
    if fn is None:
        print(f"unknown command: {verb!r} (try `help`)", file=sys.stderr)
        return 2
    return fn(args, sess.sup, sess.trace_factory)


# ---------------------------------------------------------------------------
# mode 2 — interactive REPL (prompt_toolkit)
# ---------------------------------------------------------------------------

def repl(sess: _Session) -> int:
    from prompt_toolkit import PromptSession
    from prompt_toolkit.completion import WordCompleter
    from prompt_toolkit.history import InMemoryHistory

    completer = WordCompleter(sorted(_COMMANDS) + ["help", "quit", "exit"],
                              ignore_case=True)
    psession: PromptSession = PromptSession(
        history=InMemoryHistory(), completer=completer)
    print("tdb — Theia Debug Bridge.  `help` for commands, `quit` to exit.")
    while True:
        try:
            line = psession.prompt("tdb> ").strip()
        except (EOFError, KeyboardInterrupt):
            break
        if not line:
            continue
        parts = shlex.split(line)
        verb, args = parts[0], parts[1:]
        if verb in ("quit", "exit", "q"):
            break
        if verb in ("help", "?", "h"):
            print(_HELP)
            continue
        try:
            _dispatch(sess, verb, args)
        except Exception as e:   # keep the REPL alive on a failed command
            print(f"error: {e}", file=sys.stderr)
    sess.close()
    return 0


# ---------------------------------------------------------------------------
# entry — mode 1 (argv) vs mode 2 (REPL)
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if not argv:
        return repl(_Session())

    if argv[0] in ("help", "-h", "--help"):
        print(_HELP)
        return 0

    sess = _Session()
    try:
        return _dispatch(sess, argv[0], argv[1:])
    finally:
        sess.close()


if __name__ == "__main__":
    raise SystemExit(main())

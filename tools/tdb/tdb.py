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
  info                     host facts + running build (git sha/ts) + start time
  trace [off] <node> [mt]  ConfigureTrace node on/off (msgtype "" = all kinds)
  trace off                stop ALL active traces
  trace-config             show the stored trace config (GetTraceConfig)
  loglevel [<node> [lvl]]  show all/one node's log level; with lvl, SET it live
  tracecat [--json|-c|-g]  follow the trace firehose (subscribe to log[trace]);
                           --json = NDJSON (header + decoded inner proto) per line
                           (alias: logcat — the firehose carries TRACES, not logs)
  restart <name>           RestartChild
  terminate <name>         TerminateChild (stop-and-hold: no_restart=true)
  help / quit
"""

from __future__ import annotations

import shlex
import sys
from pathlib import Path

# tdb_client + the shared command layer live next to this file.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from tdb_client import SupervisorClient, TraceClient  # noqa: E402
import tdb_commands as _cmds  # noqa: E402
from tdb_commands import _g, _COMMANDS as _SHARED_COMMANDS, _HELP  # noqa: E402,F401

REPO = Path(__file__).resolve().parents[2]


# ---------------------------------------------------------------------------
# tdb-only verb: get-snapshot (per snapshots are TIPC/artheia-only, not in the
# shared layer rtdb reuses over gRPC).
# ---------------------------------------------------------------------------

def cmd_get_snapshot(args, _sup, _tf) -> int:
    """get-snapshot <label> [--schema <file>] [--dir <snapshot_dir>]

    Trigger a per Snapshot, then decode the resulting .persnap into JSON with
    every config proto decoded — {node: {digest, config_type, config: {...}}}.

    The schema (gen-schema output, digest→config_type→shape) is generated on the
    fly from system/system.art unless --schema points at a staged one. The
    snapshot dir defaults to /tmp/theia/dbbackup (per's snapshot_dir param)."""
    import json as _json
    from tdb_client import PerClient as _PerClient

    label = next((a for a in args if not a.startswith("--")), "tdb")
    schema_path = _opt(args, "--schema")
    snap_dir = _opt(args, "--dir") or "/tmp/theia/dbbackup"

    # Schema: use a staged one, else generate from the system .art into a temp.
    if schema_path is None:
        import tempfile
        sys.path.insert(0, str(REPO / "artheia"))
        from artheia.model import parse_file as _pf
        from artheia.generators.config_schema import generate_config_schema
        art = REPO / "system" / "system.art"
        schema_path = str(Path(tempfile.gettempdir()) / "tdb_config_schema.json")
        generate_config_schema(_pf(str(art)), schema_path)

    per = _PerClient.from_workspace(REPO)
    try:
        rep = per.snapshot(label)
        if _g(rep, "status") != 0:
            print(f"Snapshot failed: {_g(rep, 'message')}", file=sys.stderr)
            return 1
        persnap = Path(snap_dir) / f"{label}.persnap"
        if not persnap.exists():
            print(f"snapshot file not found: {persnap} "
                  f"(is per on this host? snapshot_dir match?)", file=sys.stderr)
            return 1
        decoded = per.decode_snapshot(persnap, schema_path)
        print(_json.dumps({"label": label, "nodes": decoded}, indent=2,
                          default=lambda b: b.hex() if isinstance(b, bytes) else b))
        return 0
    finally:
        per.stop()


def _opt(args, name):
    """Read `--name <value>` from a flat arg list; None if absent."""
    if name in args:
        i = args.index(name)
        if i + 1 < len(args):
            return args[i + 1]
    return None


# tdb's command map = the shared verbs + the TIPC-only get-snapshot.
_COMMANDS = dict(_SHARED_COMMANDS)
_COMMANDS["get-snapshot"] = cmd_get_snapshot


# ---------------------------------------------------------------------------
# lazy clients (connect on first use so `tdb help` needs no supervisor)
# ---------------------------------------------------------------------------

class _Session:
    def __init__(self, instance: int = 0) -> None:
        self._sup = None
        self._instance = instance      # which supervisor (TIPC instance) to target

    @property
    def sup(self) -> SupervisorClient:
        if self._sup is None:
            self._sup = SupervisorClient.from_workspace(REPO, instance=self._instance)
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

    # -i / --instance <n>[,<n>...] : which supervisor(s) to target by TIPC
    # instance (central=0, compute=1 on a shared host TIPC namespace). A list
    # (`-i 0,1`) runs the command against each, with a header. Parsed before the
    # verb so the rest of the CLI is unchanged.
    instances = [0]
    if argv and argv[0] in ("-i", "--instance"):
        if len(argv) < 2:
            print("tdb: -i needs an instance (e.g. -i 1 or -i 0,1)", file=sys.stderr)
            return 2
        instances = [int(x) for x in argv[1].split(",") if x != ""]
        argv = argv[2:]

    if not argv:
        return repl(_Session(instances[0]))

    if argv[0] in ("help", "-h", "--help"):
        print(_HELP)
        return 0

    rc = 0
    for inst in instances:
        if len(instances) > 1:
            print(f"=== supervisor instance {inst} ===")
        sess = _Session(inst)
        try:
            rc = _dispatch(sess, argv[0], argv[1:]) or rc
        finally:
            sess.close()
    return rc


if __name__ == "__main__":
    raise SystemExit(main())

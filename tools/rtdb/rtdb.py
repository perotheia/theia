#!/usr/bin/env python3
# Installed on PATH as `rtdb` via a .venv/bin/rtdb symlink that `source env.sh`
# creates (alongside `tdb`), mirroring tools/tdb/tdb.py.
"""rtdb — the Remote Theia Debug Bridge (tdb over gRPC).

Same verbs as tdb, but driven over gRPC to services/com's SupervisorView
instead of local TIPC — so an operator OUTSIDE the DMZ can observe + control
the system over an IP connection (com is the gRPC↔Theia proxy). The command +
render layer is SHARED with tdb (tools/tdb/tdb_commands), so `rtdb ps` and
`tdb ps` print identically; only the transport client differs.

Two modes, like tdb / adb:
  1. One-shot:   rtdb ps                       (against 127.0.0.1:7700)
                 rtdb --target host:7700 ps
                 rtdb trace sm CAST_OUT
  2. REPL:       rtdb                           (no command)

Verbs (identical to tdb, minus get-snapshot which is TIPC/per-only):
  apps [--follow [s]]      the supervisor tree (hierarchy; GUI Applications)
  ps [--follow [s]]        flat Linux-ps list: PID/TID/name (GUI Processes)
  supervisor / info        host facts (GetSystemInfo)
  trace ...                ConfigureTrace (control path: rtdb → com → supervisor)
  trace-config             stored trace config (GetTraceConfig)
  loglevel [<node> [lvl]]  read / set a node's log level
  tracecat [--json]        follow the TRACE stream (com TraceStream gRPC :7710)
  logcat [<tag-glob>:<lvl> ...] [--json]
                           follow the LOG stream (com LogStream gRPC :7711);
                           filter adb-style, e.g. `logcat MyApp/counter:V *:E`
  restart / terminate      child lifecycle
  help / quit
"""
from __future__ import annotations

import shlex
import sys
from pathlib import Path

# rtdb_client + the shared command layer.
sys.path.insert(0, str(Path(__file__).resolve().parent))           # rtdb_client
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tdb"))  # tdb_commands
from rtdb_client import (  # noqa: E402
    SupervisorClient, TraceClient, PerClient, LogClient, NmClient,
)
from tdb_commands import _COMMANDS as _SHARED_COMMANDS, _HELP, _g  # noqa: E402
import shlex  # noqa: E402,F811  (re-import safe; used by repl)

_DEFAULT_TARGET = "127.0.0.1:7700"


# ---------------------------------------------------------------------------
# rtdb-only verbs: per (persistency) proxy via com's PerView gRPC. tdb reaches
# per directly via the probe (get-snapshot); rtdb reaches it through com.
# ---------------------------------------------------------------------------

def cmd_schemas(args, sup, _tf) -> int:
    """schemas [<config_type>] — per's schema registry via com PerView."""
    target = getattr(sup, "_target", _DEFAULT_TARGET)
    pc = PerClient(target)
    try:
        ct = args[0] if args else ""
        rows = pc.list_schemas(ct, timeout=4.0)
        if not rows:
            print("(no schemas registered)")
            return 0
        for config_type, digest in rows:
            print(f"{config_type:40} {digest}")
        return 0
    finally:
        pc.stop()


def cmd_snapshot(args, sup, _tf) -> int:
    """snapshot [<label>] — trigger a per config backup via com PerView."""
    target = getattr(sup, "_target", _DEFAULT_TARGET)
    pc = PerClient(target)
    try:
        label = args[0] if args else "rtdb"
        status, message, _rev = pc.snapshot(label, timeout=5.0)
        print(f"snapshot {label!r} -> status={status}  {message}")
        return 0 if status == 0 else 1
    finally:
        pc.stop()


def cmd_wifi(args, sup, _tf) -> int:
    """wifi [<iface>] [--status] — nm's wifi view via com NmView gRPC.

    Default: the visible APs + association (strongest first, * = associated).
    --status: just the readiness ladder snapshot (state + carrier/addr/vpn)."""
    import grpc  # local import — only the wifi verb needs the error type
    target = getattr(sup, "_target", _DEFAULT_TARGET)
    nc = NmClient(target)
    try:
        if "--status" in args:
            st = nc.get_status(timeout=4.0)
            print(f"state={st['state_name']} iface={st['interface'] or '(auto)'} "
                  f"carrier={int(st['has_carrier'])} addr={int(st['has_address'])} "
                  f"vpn={int(st['vpn_up'])}")
            return 0
        iface = next((a for a in args if not a.startswith("-")), "")
        rep = nc.wifi_scan(iface, timeout=8.0)
        name = rep["interface"] or "(no wireless iface)"
        if rep["associated"]:
            print(f"{name}: associated → {rep['assoc_ssid'] or '(hidden)'} "
                  f"[{rep['assoc_bssid']}]")
        else:
            print(f"{name}: not associated")
        bss = sorted(rep["bss"], key=lambda b: b["signal_dbm"], reverse=True)
        for b in bss:
            star = "*" if (rep["associated"] and b["bssid"] == rep["assoc_bssid"]) else " "
            print(f" {star} {(b['ssid'] or '(hidden)'):<22} {b['bssid']:<18} "
                  f"{b['signal_dbm']:>4}d {b['freq_mhz']:>5}MHz  {b['security']}")
        return 0
    except grpc.RpcError as e:
        # nm (NmDaemon) not up / not yet linked → com's NmView returns UNAVAILABLE.
        # A clean message, not a traceback.
        code = e.code().name if hasattr(e, "code") else "?"
        print(f"wifi: {code} — {e.details() if hasattr(e, 'details') else e}")
        print("  (is nm running on the target, and com linked to it? com links nm "
              "at startup — restart com if nm came up later.)")
        return 1
    finally:
        nc.stop()


# rtdb's command map = the shared verbs + the PerView-only schemas/snapshot +
# the NmView `wifi` verb (tdb has wifi over TIPC; rtdb reaches it through com).
_COMMANDS = dict(_SHARED_COMMANDS)
_COMMANDS["schemas"]  = cmd_schemas
_COMMANDS["snapshot"] = cmd_snapshot
_COMMANDS["wifi"]     = cmd_wifi

# rtdb's help is the shared one with the tdb-specific intro line swapped + the
# transport note. We just print the shared body (verb list is identical).
_RTDB_HELP = _HELP.replace("tdb — Theia Debug Bridge.",
                           "rtdb — Remote Theia Debug Bridge (over gRPC to com).")


# ---------------------------------------------------------------------------
# lazy gRPC clients (connect on first use so `rtdb help` needs no com)
# ---------------------------------------------------------------------------

class _Session:
    def __init__(self, target: str = _DEFAULT_TARGET) -> None:
        self._sup = None
        self._target = target

    @property
    def sup(self) -> SupervisorClient:
        if self._sup is None:
            self._sup = SupervisorClient(self._target)
        return self._sup

    def trace_factory(self) -> TraceClient:
        # tracecat → com's TraceForwarder TraceStream gRPC. Default :7710.
        return TraceClient.from_workspace()

    def log_factory(self) -> LogClient:
        # logcat → com's LogForwarder LogStream gRPC. Default :7711.
        return LogClient.from_workspace()

    def close(self) -> None:
        if self._sup is not None:
            self._sup.stop()


# Verbs whose handler needs the LOG firehose factory (4th positional) instead of
# the trace one — mirrors tdb.py. Every other cmd keeps (args, sup, trace_fac).
_LOG_VERBS = {"logcat"}


def _dispatch(sess: _Session, verb: str, args: list[str]) -> int:
    fn = _COMMANDS.get(verb)
    if fn is None:
        print(f"unknown command: {verb!r} (try `help`)", file=sys.stderr)
        return 2
    if verb in _LOG_VERBS:
        return fn(args, sess.sup, sess.log_factory)
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
    print(f"rtdb — Remote Theia Debug Bridge (→ {sess._target}).  "
          "`help` for commands, `quit` to exit.")
    while True:
        try:
            line = psession.prompt("rtdb> ").strip()
        except (EOFError, KeyboardInterrupt):
            break
        if not line:
            continue
        parts = shlex.split(line)
        verb, args = parts[0], parts[1:]
        if verb in ("quit", "exit", "q"):
            break
        if verb in ("help", "?", "h"):
            print(_RTDB_HELP)
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

    # --target host:port : which com endpoint to drive (default localhost:7700).
    # Parsed before the verb so the rest of the CLI matches tdb exactly.
    target = _DEFAULT_TARGET
    if argv and argv[0] in ("--target", "-t"):
        if len(argv) < 2:
            print("rtdb: --target needs host:port (e.g. --target 10.0.0.5:7700)",
                  file=sys.stderr)
            return 2
        target = argv[1]
        argv = argv[2:]

    if not argv:
        return repl(_Session(target))

    if argv[0] in ("help", "-h", "--help"):
        print(_RTDB_HELP)
        return 0

    # Hidden hook: emit the verb list (one per line) for shell completion, so
    # env.sh stays in lockstep with _COMMANDS instead of a hardcoded list. Runs
    # before any com/gRPC connect.
    if argv[0] == "__complete":
        print("\n".join(_COMMANDS))
        return 0

    sess = _Session(target)
    try:
        return _dispatch(sess, argv[0], argv[1:])
    finally:
        sess.close()


if __name__ == "__main__":
    raise SystemExit(main())

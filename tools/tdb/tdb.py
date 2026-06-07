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
import time
from datetime import datetime
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
    # `ps --follow [interval]` streams the tree by POLLING GetTree on an
    # interval (default 1s) and re-rendering — the pull model (the supervisor's
    # firehose has no remote egress; GetTree is the live source, same as a plain
    # `ps`). com's gRPC Subscribe mirrors this poll-stream.
    follow = "--follow" in args or "-f" in args
    if not follow:
        reply = sup.get_tree(timeout=3.0)
        print(_render_tree(list(_g(reply, "children", []) or [])))
        return 0
    nums = [a for a in args if not a.startswith("-")]
    interval = float(nums[0]) if nums else 1.0
    try:
        while True:
            reply = sup.get_tree(timeout=3.0)
            sys.stdout.write("\x1b[2J\x1b[H")   # clear + home
            print(f"tdb ps --follow  (every {interval}s, Ctrl-C to stop)\n")
            print(_render_tree(list(_g(reply, "children", []) or [])))
            sys.stdout.flush()
            time.sleep(interval)
    except KeyboardInterrupt:
        return 0


def cmd_supervisor(args, sup, _tf) -> int:
    info = sup.get_system_info(timeout=3.0)
    for k in ("hostname", "kernel", "os_pretty_name", "cpu_count",
              "total_ram_kb", "uptime_sec"):
        print(f"{k:16} {_g(info, k, '')}")
    return 0


def _fmt_epoch_ms(ts_ms: int) -> str:
    """Epoch-milliseconds → 'DD/MM/YY HH:MM:SS' local time. 0 → ''."""
    if not ts_ms:
        return ""
    return datetime.fromtimestamp(ts_ms / 1000.0).strftime("%d/%m/%y %H:%M:%S")


def _fmt_ram_mb(ram_kb: int) -> str:
    """Total RAM kB → human MB/GB. e.g. 32538708 → '31044 MB (30.3 GB)'."""
    if not ram_kb:
        return ""
    mb = ram_kb / 1024.0
    return f"{mb:,.0f} MB ({mb / 1024.0:.1f} GB)"


def _fmt_uptime(sec: int) -> str:
    """Seconds → 'Dd Hh Mm' (largest non-zero unit first). e.g.
    269681 → '3d 2h 54m'."""
    if not sec:
        return ""
    days, rem = divmod(int(sec), 86400)
    hours, rem = divmod(rem, 3600)
    mins = rem // 60
    parts = []
    if days:
        parts.append(f"{days}d")
    if hours or days:
        parts.append(f"{hours}h")
    parts.append(f"{mins}m")
    return " ".join(parts)


def cmd_info(args, sup, _tf) -> int:
    """Full host + BUILD facts (GetSystemInfo) — like `supervisor`, plus the
    running build's git sha / timestamp (THEIA_GIT_SHA / THEIA_BUILD_TIMESTAMP,
    baked at build time) and when this supervisor process started."""
    info = sup.get_system_info(timeout=3.0)
    rows = [
        ("hostname",       _g(info, "hostname", "")),
        ("kernel",         _g(info, "kernel", "")),
        ("os",             _g(info, "os_pretty_name", "")),
        ("cpus",           _g(info, "cpu_count", "")),
        ("ram",            _fmt_ram_mb(_g(info, "total_ram_kb", 0))),
        ("uptime",         _fmt_uptime(_g(info, "uptime_sec", 0))),
        ("theia_git_sha",  _g(info, "theia_git_sha", "") or "(unstamped)"),
        ("build_ts",       _g(info, "build_timestamp", "") or "(unstamped)"),
        ("started",        _fmt_epoch_ms(_g(info, "start_timestamp_ms", 0))),
    ]
    for k, v in rows:
        print(f"{k:16} {v}")
    return 0


# TraceKind ordinal ↔ name (platform_runtime.TraceKind). A node's trace filter
# is a BITMASK — several kinds can be on at once; OTHER (0) is the catch-all
# "all kinds" sentinel (shown as ALL). The supervisor stores a SET of enabled
# kinds per node, so a node yields one read-back row per enabled kind.
_KIND_NAMES = {0: "OTHER", 1: "CAST_OUT", 2: "CAST_IN",
               3: "CALL_OUT", 4: "CALL_IN", 5: "STATEM"}
_KIND_ORD = {v: k for k, v in _KIND_NAMES.items()}


def _cfg_kind(c) -> int:
    """A read-back TraceConfig row nests the kind at trace_ctrl.kind
    (TraceConfig{ target_node; TraceControlPush trace_ctrl{ kind; enabled } }),
    not at the top level. Returns the TraceKind ordinal (0 = OTHER)."""
    return int(_g(_g(c, "trace_ctrl"), "kind", 0) or 0)


def _active_by_node(sup) -> dict[str, set[int]]:
    """node → set of enabled TraceKind ordinals (one read-back row per kind)."""
    rows = list(_g(sup.get_trace_config(timeout=3.0), "configs", []) or [])
    out: dict[str, set[int]] = {}
    for c in rows:
        node = _g(c, "target_node")
        if node:
            out.setdefault(node, set()).add(_cfg_kind(c))
    return out


def _fmt_kinds(kinds: set[int]) -> str:
    """A kind-set → display string. {0} (catch-all) → 'ALL'; else the kind
    names joined, low ordinal first."""
    if kinds == {0}:
        return "ALL"
    return "|".join(_KIND_NAMES.get(k, "?") for k in sorted(kinds))


def cmd_trace(args, sup, _tf) -> int:
    """trace                     — every node with an active trace + its kinds
       trace off                 — stop ALL traces
       trace <node>              — that node's trace kinds (or off)
       trace <node> <KIND>       — ADD KIND to <node>'s trace (live, no restart)
       trace <node> <KIND> off   — remove just that KIND
       trace <node> off          — stop <node>'s trace entirely
       trace <node> OTHER        — catch-all: trace EVERY kind
    KIND ∈ CAST_OUT|CAST_IN|CALL_OUT|CALL_IN|STATEM|OTHER (case-insensitive).
    Kinds accumulate — a node can trace several at once."""
    # `trace off` (no node) — stop everything.
    if len(args) == 1 and args[0].lower() == "off":
        return _trace_off_all(sup)

    # SET / per-kind toggle: <node> <KIND|off> [off]
    if len(args) >= 2:
        node, kind_arg = args[0], args[1].upper()
        # `trace <node> off` — disable the whole node (catch-all disable).
        if kind_arg == "OFF":
            kind, enabled = 0, False
        elif kind_arg in _KIND_ORD:
            kind = _KIND_ORD[kind_arg]
            # optional 3rd token off/on toggles just this kind (default on/add).
            enabled = not (len(args) >= 3 and args[2].lower() == "off")
        else:
            print(f"bad kind {args[1]!r} "
                  f"(use {'/'.join(_KIND_ORD)} or off)", file=sys.stderr)
            return 2
        rep = sup.configure_trace(target_node=node, enabled=enabled,
                                  kind=kind, timeout=3.0)
        status = _g(rep, "status")
        msg = _g(rep, "message", "")
        if status == 0:
            # Echo the node's RESULTING kind-set so the accumulation is visible.
            kinds = _active_by_node(sup).get(node, set())
            state = _fmt_kinds(kinds) if kinds else "off"
            print(f"trace {node} -> {state}"
                  f"{(' (' + msg + ')') if msg else ''}")
            return 0
        print(f"trace {node} -> FAILED (status={status}): {msg or 'rejected'}",
              file=sys.stderr)
        return 1

    # GET (all, or one node) — mirrors `loglevel` read-back.
    active = _active_by_node(sup)
    if args:                      # trace <node> — that node's state
        node = args[0]
        print(f"{node:18} {_fmt_kinds(active[node]) if node in active else 'off'}")
        return 0
    # trace (no args) — every node with an active trace.
    if not active:
        print("(no active traces)")
        return 0
    for node in sorted(active):
        print(f"{node:18} {_fmt_kinds(active[node])}")
    return 0


def _trace_off_all(sup) -> int:
    """`trace off` with no node: disable EVERY stored trace config entry.

    Read the supervisor's current trace config and ConfigureTrace(enabled=False)
    each target_node, so every node the supervisor was pushing trace to gets a
    disable.
    """
    tc = sup.get_trace_config(timeout=3.0)
    cfgs = list(_g(tc, "configs", []) or [])
    if not cfgs:
        print("trace off: nothing active")
        return 0
    rc = 0
    for c in cfgs:
        node = _g(c, "target_node")
        if not node:
            continue
        rep = sup.configure_trace(target_node=node,
                                  enabled=False, kind=0, timeout=3.0)
        status = _g(rep, "status")
        if status == 0:
            print(f"trace off: {node} -> ok")
        else:
            print(f"trace off: {node} -> FAILED (status={status}): "
                  f"{_g(rep, 'message', 'rejected')}", file=sys.stderr)
            rc = 1
    return rc


def cmd_trace_config(args, sup, _tf) -> int:
    tc = sup.get_trace_config(timeout=3.0)
    cfgs = list(_g(tc, "configs", []) or [])
    if not cfgs:
        print("(no trace config)")
        return 0
    for c in cfgs:
        print(f"{_g(c, 'target_node'):18} "
              f"{_KIND_NAMES.get(_cfg_kind(c), '?')}")
    return 0


_LEVEL_NAMES = {0: "trace", 1: "debug", 2: "info", 3: "warn", 4: "error"}
_LEVEL_SET = set(_LEVEL_NAMES.values())


def cmd_loglevel(args, sup, _tf) -> int:
    """loglevel                — every reporting node's effective level
       loglevel <node>         — that node's level
       loglevel <node> <level> — SET <node> to <level> (live, no restart)
    level ∈ trace|debug|info|warn|error."""
    # SET: <node> <level>
    if len(args) >= 2:
        node, level = args[0], args[1].lower()
        if level not in _LEVEL_SET:
            print(f"bad level {args[1]!r} (use {'/'.join(sorted(_LEVEL_SET))})",
                  file=sys.stderr)
            return 2
        rep = sup.configure_log_level(target_node=node, level=level, timeout=3.0)
        status = _g(rep, "status")
        msg = _g(rep, "message", "")
        if status == 0:
            print(f"loglevel {node} -> {level}"
                  f"{(' (' + msg + ')') if msg else ''}")
            return 0
        print(f"loglevel {node} -> FAILED (status={status}): {msg or 'rejected'}",
              file=sys.stderr)
        return 1

    # GET (all, or one node)
    rows = list(_g(sup.get_log_level_config(timeout=3.0), "configs", []) or [])
    want = args[0] if args else None
    shown = 0
    for r in rows:
        name = _g(r, "target_node")
        if want is not None and name != want:
            continue
        lvl = _LEVEL_NAMES.get(_g(r, "level", 2), "?")
        if _g(r, "is_override", False):
            boot = _LEVEL_NAMES.get(_g(r, "boot_level", 2), "?")
            print(f"{name:18} {lvl:6} (override; boot={boot})")
        else:
            print(f"{name:18} {lvl:6} (boot)")
        shown += 1
    if want is not None and shown == 0:
        print(f"no reporting node named {want!r}", file=sys.stderr)
        return 1
    if shown == 0:
        print("(no reporting nodes)")
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


def _fmt_epoch_ns(ts_ns: int) -> str:
    """Epoch-nanoseconds (the sender's system_clock at the trace point) →
    DD/MM/YY HH:MM:SS.mmm local time. 0 → '' (no timestamp)."""
    if not ts_ns:
        return ""
    dt = datetime.fromtimestamp(ts_ns / 1e9)
    return dt.strftime("%d/%m/%y %H:%M:%S.") + f"{dt.microsecond // 1000:03d}"


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
                # ts_ns is the SENDER node's wall-clock at the trace point
                # (system_clock epoch ns) — format it as DD/MM/YY HH:MM:SS.mmm
                # so the JSON carries the real event time, not the receive time.
                ts = _fmt_epoch_ns(rec.ts_ns)
                print(json.dumps(rec.to_dict(ts=ts), separators=(",", ":")),
                      flush=True)
                continue
            # Prefer the DECODED inner message ({value: 860}) over the raw
            # base64 payload in the envelope JSON. content is None when the
            # record has no payload (e.g. Dispatch) or the type didn't resolve.
            body = _fmt_content(rec.content) if rec.content else ""
            kind = getattr(rec, "kind", "") or "?"
            print(f"{rec.ts_ns:>16} {rec.src:>16} {kind:<9} {rec.msg_type:<22} "
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
    "info": cmd_info,
    "trace": cmd_trace,
    "trace-config": cmd_trace_config,
    "loglevel": cmd_loglevel,
    "restart": cmd_restart,
    "terminate": cmd_terminate,
    "logcat": cmd_logcat,
    "get-snapshot": cmd_get_snapshot,
}

_HELP = """tdb — Theia Debug Bridge. commands:
  ps                       list the supervisor tree
  supervisor               supervisor host facts
  info                     host facts + running build (git sha / ts) + start time
  trace                    list every node with an active trace + its kinds
  trace off                stop ALL active traces
  trace <node>             show that node's trace kinds (or off)
  trace <node> <KIND>      ADD KIND to node (CAST_OUT|CAST_IN|CALL_OUT|CALL_IN|
                           STATEM|OTHER=all); kinds accumulate
  trace <node> <KIND> off  remove just that KIND; `trace <node> off` stops all
  trace-config             show stored trace config
  loglevel [<node> [lvl]]  show all/one node's log level; with lvl, SET it live
  logcat [--json]          follow the trace firehose (--json = NDJSON)
  restart <name>           restart a child
  terminate <name>         stop-and-hold a child
  help                     this help
  quit / exit              leave the REPL"""


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

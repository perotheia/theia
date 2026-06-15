#!/usr/bin/env python3
"""tdb_commands — the shared verb + render layer for tdb (TIPC, local) and
rtdb (gRPC, remote).

Every cmd_* takes (args, sup, trace_factory) and uses ONLY:
  - sup: an object with the SupervisorClient method surface (get_tree,
    get_system_info, configure_trace, get_trace_config, configure_log_level,
    get_log_level_config, restart_child, terminate_hold) returning plain dicts;
  - trace_factory(): a zero-arg callable returning a trace client with
    .records(timeout) / .stop() (tracecat).
So the SAME command bodies serve both transports — tdb passes TIPC-backed
clients, rtdb passes gRPC-backed ones. _render_tree etc. live here so local +
remote print identically.

tdb keeps its own get-snapshot verb (per snapshots are TIPC/artheia-only).
"""
from __future__ import annotations

import json
import sys
import time
from datetime import datetime

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

def _fmt_uptime_ms(ms: int) -> str:
    """uptime ms → 'Dd Hh Mm Ss' (largest non-zero unit first)."""
    if not ms:
        return "0s"
    sec = int(ms // 1000)
    d, rem = divmod(sec, 86400)
    h, rem = divmod(rem, 3600)
    m, s = divmod(rem, 60)
    parts = []
    if d:
        parts.append(f"{d}d")
    if h or d:
        parts.append(f"{h}h")
    if m or h or d:
        parts.append(f"{m}m")
    parts.append(f"{s}s")
    return " ".join(parts)


def _fmt_kb(kb: int) -> str:
    """kB → human MB (with the raw kB). e.g. 17604 → '17.2 MB'."""
    if not kb:
        return "0"
    mb = kb / 1024.0
    return f"{mb:,.1f} MB" if mb >= 1.0 else f"{kb} kB"


def _find_child(reply, name: str):
    """The ChildState row for a worker/node named `name`, or None."""
    for ch in (_g(reply, "children", []) or []):
        if _g(ch, "name") == name:
            return ch
    return None


def cmd_ps(args, sup, _tf) -> int:
    # `ps`                  — the whole supervisor tree
    # `ps <name>`           — metrics for one process/node (uptime/cpu/mem/...)
    # `ps --follow [int]`   — poll the tree on an interval (pull model: the
    #                         supervisor firehose has no remote egress; GetTree
    #                         is the live source. com's gRPC Subscribe mirrors it)
    # `ps <name> --follow`  — poll one process's metrics
    follow = "--follow" in args or "-f" in args
    positional = [a for a in args if not a.startswith("-")]
    # A non-numeric positional is a process NAME (numeric = the follow interval).
    name = next((a for a in positional if not _is_number(a)), None)
    nums = [a for a in positional if _is_number(a)]
    interval = float(nums[0]) if nums else 1.0

    def render() -> str:
        reply = sup.get_tree(timeout=3.0)
        if name:
            ch = _find_child(reply, name)
            if ch is None:
                return f"no process/node named {name!r} in the supervisor tree"
            return _render_proc(ch)
        return _render_tree(list(_g(reply, "children", []) or []))

    if not follow:
        print(render())
        return 0
    label = f"ps {name}" if name else "ps"
    try:
        while True:
            sys.stdout.write("\x1b[2J\x1b[H")   # clear + home
            print(f"tdb {label} --follow  (every {interval}s, Ctrl-C to stop)\n")
            print(render())
            sys.stdout.flush()
            time.sleep(interval)
    except KeyboardInterrupt:
        return 0


def _is_number(s: str) -> bool:
    try:
        float(s)
        return True
    except ValueError:
        return False


def _render_proc(ch) -> str:
    """One process/node's metrics, key/value (the `ps <name>` detail view).
    Reads the ChildState fields the supervisor now fills from its /proc sample
    (uptime_ms, cpu_pct hundredths-of-%, rss/shared/data/vsz kB, threads)."""
    kind = {1: "supervisor", 0: "process", 2: "node"}.get(_g(ch, "kind"), "node")
    state = _STATE.get(_g(ch, "state", 0), str(_g(ch, "state")))
    pid = _g(ch, "pid", -1)
    rows = [
        ("name",        _g(ch, "name")),
        ("kind",        kind),
        ("parent",      _g(ch, "parent_name", "")),
        ("state",       state),
        ("pid",         pid if pid and pid > 0 else "—"),
        ("uptime",      _fmt_uptime_ms(_g(ch, "uptime_ms", 0))),
        ("cpu",         f"{_g(ch, 'cpu_pct', 0) / 100.0:.2f} %"),
        ("rss",         _fmt_kb(_g(ch, "rss_kb", 0))),
        ("shared",      _fmt_kb(_g(ch, "shared_kb", 0))),
        ("data",        _fmt_kb(_g(ch, "data_kb", 0))),
        ("vsz",         _fmt_kb(_g(ch, "vsz_kb", 0))),
        ("threads",     _g(ch, "threads", 0)),
        ("restarts",    _g(ch, "restart_count", 0)),
        ("last_exit",   _g(ch, "last_exit_code", 0)),
    ]
    return "\n".join(f"{k:12} {v}" for k, v in rows)


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


def cmd_tracecat(args, _sup, trace_factory) -> int:
    # Follows the TRACE firehose (subscribe to log[trace], decode + print) —
    # NOT logs. Named `tracecat`; `logcat` kept as a back-compat alias. A real
    # log-tailing `logcat` (svc/log syslog sink watcher) is a separate feature.
    # -g get ring size / -c clear are firehose-control follow-ups; the default
    # is "follow live records".
    if "-g" in args or "-c" in args:
        print("tracecat -g/-c (ring size / clear) — not wired yet", file=sys.stderr)
        return 2
    # --json: one JSON object per line (NDJSON) — full header + decoded inner
    # proto — for piping into jq / a log pipeline. No human banner on stdout.
    as_json = "--json" in args
    trace = trace_factory()
    if not as_json:
        print("tracecat: following trace firehose (Ctrl-C to stop) ...")
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
            # Strip the proto enum's "TraceKind_" prefix (gen-proto names enum
            # members <Enum>_<NAME> for nanopb compat) — show just CALL_OUT.
            kind = getattr(rec, "kind", "") or "?"
            if kind.startswith("TraceKind_"):
                kind = kind[len("TraceKind_"):]
            # STATEM rows carry the transition in from_state/to_state — show it
            # as `from→to` (the event is in msg_type). e.g. OFF→STARTING.
            frm = getattr(rec, "from_state", "") or ""
            to = getattr(rec, "to_state", "") or ""
            if frm or to:
                # FSM data (OTP Data term) rides on STATEM rows too — append it
                # after the transition: `IDLE→PROCESSING data={visits: 1, ...}`.
                data = getattr(rec, "data", None)
                data_str = f" data={_fmt_content(data)}" if data else ""
                body = (f"{frm}→{to}{data_str}"
                        + (f" {body}" if body else "")).strip()
            # ts_ns is system_clock epoch ns (Tracer.hh) — the SAME clock logcat
            # uses, so format it the same way (DD/MM/YY HH:MM:SS.mmm) for a
            # consistent timeline across both firehoses.
            ts = _fmt_epoch_ns(rec.ts_ns)
            print(f"{ts} {rec.src} {kind} {rec.msg_type} "
                  f"corr={rec.corr_id}{(' ' + body) if body else ''}",
                  flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        trace.stop()
    return 0


def cmd_logcat(args, _sup, log_factory) -> int:
    # Follows the LOG firehose — the node log LINES (Logger.hh output), NOT the
    # trace records (that's `tracecat`). Subscribes to log[logging], which spins
    # up the tailer on this first subscriber; each line is decoded + printed
    # adb-style. Subscriber-side <tag-glob>:<level> filter (the hose is dumb).
    #
    #   tdb logcat                     follow everything
    #   tdb logcat *:E                 errors only, every tag
    #   tdb logcat MyApp/counter:V *:E verbose for MyApp's counter, errors else
    #   tdb logcat --json              one JSON object per line (NDJSON)
    from artheia.observer.logcat_filter import LogcatFilter

    as_json = "--json" in args
    specs = [a for a in args if a != "--json"]
    try:
        filt = LogcatFilter.parse(specs)
    except ValueError as e:
        print(f"logcat: {e}", file=sys.stderr)
        return 2

    log = log_factory()
    if not as_json:
        print("logcat: following log firehose (Ctrl-C to stop) ...")
    try:
        for rec in log.records(timeout=600.0):
            if not filt.keep(tag=rec.tag, node=rec.node, level_ord=rec.level_ord):
                continue
            if as_json:
                ts = _fmt_epoch_ns(rec.ts_ns)
                print(json.dumps(rec.to_dict(ts=ts), separators=(",", ":")),
                      flush=True)
                continue
            # adb-style: "<ts> <LEVEL> <tag>/<node> <msg>". rec.line is just the
            # message body (the hub strips the file line's own ts+[LEVEL] prefix
            # into ts_ns/level), so tdb renders ts + level once from the fields.
            ts = _fmt_epoch_ns(rec.ts_ns)
            print(f"{ts:<21} {rec.level_code} {rec.tag}/{rec.node} {rec.line}",
                  flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        log.stop()
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
    "tracecat": cmd_tracecat,
    "logcat": cmd_logcat,     # the LOG firehose (node log lines); tracecat = TRACES
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
  tracecat [--json]        follow the TRACE firehose (--json = NDJSON)
  logcat [<tag-glob>:<lvl> ...] [--json]
                           follow the LOG firehose (node log lines); filter
                           adb-style, e.g. `logcat MyApp/counter:V *:E`
  restart <name>           restart a child
  terminate <name>         stop-and-hold a child
  help                     this help
  quit / exit              leave the REPL"""

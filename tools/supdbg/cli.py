# supdbg.cli — one-shot subcommands wrapping supdbg.client.Client.
#
# Usage:
#     python -m supdbg --target 127.0.0.1:7700 tree
#     python -m supdbg --target 127.0.0.1:7700 restart demo_p1
#     python -m supdbg --target 127.0.0.1:7700 terminate demo_p1
#     python -m supdbg --target 127.0.0.1:7700 watch
#     python -m supdbg                          # interactive REPL
#
# --target defaults to 127.0.0.1:7700 (central in the docker-compose
# rig). Use --machines <yaml> for multi-machine setups.

from __future__ import annotations

import argparse
import json
import sys
import typing as t

import grpc

from .client import Client, EventKind
from . import printers


def _friendly_grpc_error(e: grpc.RpcError, target: str) -> str:
    """Translate a raw grpc.RpcError into a one-line operator message.
    Used to turn 5-frame tracebacks into readable CLI errors."""
    code = e.code() if hasattr(e, "code") else None
    if code == grpc.StatusCode.UNAVAILABLE:
        return (f"supdbg: cannot reach gRPC bridge at {target} "
                f"(services/com not running, or wrong port)")
    if code == grpc.StatusCode.DEADLINE_EXCEEDED:
        return f"supdbg: deadline exceeded waiting for {target}"
    return f"supdbg: gRPC error from {target}: {code} {e.details() if hasattr(e, 'details') else ''}"


def _wrap(fn):
    """Decorator: catch grpc.RpcError on one-shot commands and emit a
    one-line stderr message + non-zero exit rather than a traceback."""
    def inner(args):
        try:
            return fn(args)
        except grpc.RpcError as e:
            print(_friendly_grpc_error(e, getattr(args, "target", "?")),
                  file=sys.stderr)
            return 3
    return inner


def _add_target(_p: argparse.ArgumentParser) -> None:
    # No-op: --target lives on the top-level parser so it can appear
    # before OR after the subcommand. Kept as a stub so subparser
    # definitions don't need to know.
    return


@_wrap
def cmd_tree(args: argparse.Namespace) -> int:
    with Client(args.target) as c:
        snap = c.tree(timeout=args.wait)
        if snap is None:
            print(f"supdbg: no snapshot from {args.target} within {args.wait}s",
                  file=sys.stderr)
            return 1
        if args.json:
            print(printers.snapshot_json(snap))
        else:
            printers.print_snapshot(snap, sys.stdout)
    return 0


@_wrap
def cmd_restart(args: argparse.Namespace) -> int:
    with Client(args.target) as c:
        reply = c.restart_child(args.child)
        return printers.print_reply(reply, sys.stdout)


@_wrap
def cmd_terminate(args: argparse.Namespace) -> int:
    with Client(args.target) as c:
        reply = c.terminate_child(args.child)
        return printers.print_reply(reply, sys.stdout)


@_wrap
def cmd_delete(args: argparse.Namespace) -> int:
    with Client(args.target) as c:
        reply = c.delete_child(args.child)
        return printers.print_reply(reply, sys.stdout)


@_wrap
def cmd_watch(args: argparse.Namespace) -> int:
    only = set(args.only or ["event", "health", "snapshot"])
    with Client(args.target) as c:
        try:
            for obs in c.subscribe():
                if obs.kind() not in only:
                    continue
                printers.print_observation(obs, sys.stdout)
        except KeyboardInterrupt:
            return 0
    return 0


@_wrap
def cmd_wait(args: argparse.Namespace) -> int:
    """Block until a matching SupervisionEvent arrives, then exit.
    Designed for shell-driven tests:

        supdbg wait --kind EXITED --child demo_p1 --timeout 5 || fail
    """
    with Client(args.target) as c:
        kind = EventKind[args.kind] if args.kind else None
        ev = c.wait_event(kind=kind, child_name=args.child, timeout=args.timeout)
        if ev is None:
            print("supdbg: timeout waiting for event", file=sys.stderr)
            return 2
        printers.print_event(ev, sys.stdout)
    return 0


def cmd_repl(args: argparse.Namespace) -> int:
    from . import repl
    return repl.run(args.target)


@_wrap
def cmd_trace_enable(args: argparse.Namespace) -> int:
    """Toggle a (node, msg_type) trace filter ON via TraceStream.Configure."""
    with Client(args.target) as c:
        r = c.configure_trace(args.node, args.msg_type, enabled=True)
        if r.status != 0:
            print(f"trace enable failed: status={r.status} message={r.message!r}")
            return 1
        print(f"trace ON: {args.node}/{args.msg_type}")
        return 0


@_wrap
def cmd_trace_disable(args: argparse.Namespace) -> int:
    """Toggle a (node, msg_type) trace filter OFF."""
    with Client(args.target) as c:
        r = c.configure_trace(args.node, args.msg_type, enabled=False)
        if r.status != 0:
            print(f"trace disable failed: status={r.status} message={r.message!r}")
            return 1
        print(f"trace OFF: {args.node}/{args.msg_type}")
        return 0


@_wrap
def cmd_trace_stream(args: argparse.Namespace) -> int:
    """Subscribe to the TraceStream and print every record.

    With --decode, looks up libtrace_decoder.so via rf_theia and
    pretty-prints the JSON-decoded payload alongside each record.
    Without --decode, prints node/msg_type/corr_id/ts_ns and the raw
    payload byte count.
    """
    decoder = None
    if args.decode:
        try:
            # rf_theia ships the ctypes wrapper (#356).
            from rf_theia.adapters.trace_decoder import open_default
            decoder = open_default()
            print(f"# loaded libtrace_decoder.so ({decoder.registered_count()} protos)")
        except Exception as e:
            print(f"# WARNING: decoder unavailable ({e}); falling back to raw")

    with Client(args.target) as c:
        try:
            for rec, decoded in c.subscribe_traces(decoder=decoder):
                if decoded is not None:
                    print(f"[{rec.ts_ns}] {rec.node_name} {rec.msg_type} "
                          f"corr={rec.corr_id} {decoded}")
                else:
                    print(f"[{rec.ts_ns}] {rec.node_name} {rec.msg_type} "
                          f"corr={rec.corr_id} ({len(rec.payload)}B raw)")
        except KeyboardInterrupt:
            return 0
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="supdbg",
        description=(
            "Text-mode supervisor debugger. Targets the services/com "
            "gRPC bridge on a machine. Modeled (loosely) on Erlang's "
            "dbg module — see `repl` subcommand for the interactive "
            "letter-command shell."
        ),
    )
    # Top-level --target so it works in both `supdbg -t X tree` and
    # `supdbg tree -t X` forms; subparsers consult args.target.
    p.add_argument(
        "--target", "-t",
        default="127.0.0.1:7700",
        help="host:port of services/com gRPC bridge (default 127.0.0.1:7700)",
    )
    sub = p.add_subparsers(dest="cmd")
    sub.required = False  # no subcommand → REPL

    pt = sub.add_parser("tree", help="print current supervisor tree snapshot")
    _add_target(pt)
    pt.add_argument("--json", action="store_true", help="emit JSON instead of text")
    pt.add_argument("--wait", type=float, default=3.0,
                    help="seconds to wait for a snapshot (default 3)")
    pt.set_defaults(func=cmd_tree)

    pr = sub.add_parser("restart", help="restart a child")
    _add_target(pr)
    pr.add_argument("child", help="child name (e.g. demo_p1)")
    pr.set_defaults(func=cmd_restart)

    pt2 = sub.add_parser("terminate", help="terminate a child (one-shot)")
    _add_target(pt2)
    pt2.add_argument("child")
    pt2.set_defaults(func=cmd_terminate)

    pd = sub.add_parser("delete", help="delete a child spec (cannot be restarted)")
    _add_target(pd)
    pd.add_argument("child")
    pd.set_defaults(func=cmd_delete)

    pw = sub.add_parser("watch", help="stream observations forever (Ctrl-C to stop)")
    _add_target(pw)
    pw.add_argument("--only", action="append", choices=["event", "health", "snapshot"],
                    help="filter kinds (repeatable); default = all")
    pw.set_defaults(func=cmd_watch)

    pwait = sub.add_parser("wait", help="block until a matching SupervisionEvent arrives")
    _add_target(pwait)
    pwait.add_argument("--kind", choices=[k.name for k in EventKind])
    pwait.add_argument("--child")
    pwait.add_argument("--timeout", type=float, default=5.0)
    pwait.set_defaults(func=cmd_wait)

    prepl = sub.add_parser("repl", help="interactive dbg-style REPL")
    _add_target(prepl)
    prepl.set_defaults(func=cmd_repl)

    # ---- trace (#360) ----------------------------------------------------
    ptrace = sub.add_parser(
        "trace",
        help="control TraceStream — enable/disable filters, stream records",
    )
    trace_sub = ptrace.add_subparsers(dest="trace_cmd")
    trace_sub.required = True

    pte = trace_sub.add_parser(
        "enable", help="enable trace for (node, msg_type)")
    _add_target(pte)
    pte.add_argument("node", help="target node name (e.g. SmDaemon)")
    pte.add_argument("msg_type", help="message type (e.g. SmStateMsg)")
    pte.set_defaults(func=cmd_trace_enable)

    ptd = trace_sub.add_parser(
        "disable", help="disable trace for (node, msg_type)")
    _add_target(ptd)
    ptd.add_argument("node")
    ptd.add_argument("msg_type")
    ptd.set_defaults(func=cmd_trace_disable)

    pts = trace_sub.add_parser(
        "stream", help="stream trace records (Ctrl-C to stop)")
    _add_target(pts)
    pts.add_argument(
        "--decode", action="store_true",
        help="decode payload via libtrace_decoder.so (#356); falls back "
             "to raw bytes if the .so isn't available",
    )
    pts.set_defaults(func=cmd_trace_stream)

    # default = repl
    p.set_defaults(func=cmd_repl, target="127.0.0.1:7700")
    return p


def main(argv: t.Optional[t.Sequence[str]] = None) -> int:
    p = build_parser()
    args = p.parse_args(argv)
    return args.func(args) or 0


if __name__ == "__main__":
    raise SystemExit(main())

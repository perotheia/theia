# supdbg.repl — interactive supervisor debugger shell.
#
# Modeled (loosely) on Erlang's dbg REPL: short letter commands, an
# always-on watch stream that can be paused, multi-machine targeting.
#
# Commands (h to list):
#   i             tree (snapshot of current children)
#   r <name>      restart child
#   t <name>      terminate child (one-shot)
#   d <name>      delete child spec
#   w             toggle background watch (events + health)
#   wt            toggle watch of TreeSnapshot too (noisy)
#   n <host:port> switch target machine
#   ls            list known machines (just shows current target)
#   h, ?          help
#   q             quit
#
# The watch tail runs on a daemon thread; toggling w stops/starts it.
# Output from the tail is mixed with the prompt — that's intentional
# (dbg does the same).

from __future__ import annotations

import sys
import shlex
import threading
import time
import typing as t

from .client import Client
from . import printers


_HELP = """\
supdbg commands:
  i                  tree (current snapshot)
  r <name>           restart child
  t <name>           terminate child
  s <name> <cmd...>  start child with start_cmd (uses default kind=WORKER)
  d <name>           delete child spec
  w                  toggle event+health tail
  wt                 toggle tree-snapshot tail (noisy)
  n <host:port>      switch target machine
  ls                 show current target
  h, ?               this help
  q, exit            quit
"""


class _Tailer:
    """Background thread that reads the subscribe stream and prints
    observations. Filters can be flipped without restarting the thread.
    """

    def __init__(self, client: Client) -> None:
        self.client = client
        self._stop  = threading.Event()
        self._thr:  t.Optional[threading.Thread] = None
        self.show_events    = False
        self.show_health    = False
        self.show_snapshot  = False

    def running(self) -> bool:
        return self._thr is not None and self._thr.is_alive()

    def start(self) -> None:
        if self.running():
            return
        self._stop.clear()
        self._thr = threading.Thread(target=self._run, daemon=True)
        self._thr.start()

    def stop(self) -> None:
        self._stop.set()
        self.client.close()  # break out of the blocking iterator
        if self._thr is not None:
            self._thr.join(timeout=2)
        self._thr = None

    def _run(self) -> None:
        try:
            for obs in self.client.subscribe():
                if self._stop.is_set():
                    return
                k = obs.kind()
                if (k == "event"    and self.show_events) or \
                   (k == "health"   and self.show_health) or \
                   (k == "snapshot" and self.show_snapshot):
                    printers.print_observation(obs, sys.stdout)
        except Exception as e:  # noqa: BLE001
            if not self._stop.is_set():
                print(f"\n[tail error: {e}]", file=sys.stderr)


# -- command dispatch ------------------------------------------------

def _cmd_i(client: Client, _args: list) -> None:
    snap = client.tree(timeout=3.0)
    if snap is None:
        print("(no snapshot within 3s)")
        return
    printers.print_snapshot(snap, sys.stdout)


def _cmd_r(client: Client, args: list) -> None:
    if not args:
        print("usage: r <child>")
        return
    printers.print_reply(client.restart_child(args[0]), sys.stdout)


def _cmd_t(client: Client, args: list) -> None:
    if not args:
        print("usage: t <child>")
        return
    printers.print_reply(client.terminate_child(args[0]), sys.stdout)


def _cmd_d(client: Client, args: list) -> None:
    if not args:
        print("usage: d <child>")
        return
    printers.print_reply(client.delete_child(args[0]), sys.stdout)


def _cmd_s(client: Client, args: list) -> None:
    # Minimal start_child: name + start_cmd. Restart=PERMANENT, kind=WORKER.
    # Numeric defaults come from the .art enums; if these drift, the
    # supervisor will reject with a non-zero ControlReply.status.
    if len(args) < 2:
        print("usage: s <name> <cmd> [arg ...]")
        return
    from . import client as _c
    spec = _c._spec_pb.ChildSpec(
        name=args[0],
        start_cmd=args[1:],
        restart=1,   # PERMANENT
        type=1,      # WORKER
        shutdown=5000,
    )
    printers.print_reply(client.start_child(spec), sys.stdout)


def run(initial_target: str) -> int:
    target = initial_target
    client = Client(target)
    tailer = _Tailer(client)

    print(f"supdbg interactive — target {target}  (h for help)")
    while True:
        try:
            line = input("supdbg> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not line:
            continue
        try:
            tokens = shlex.split(line)
        except ValueError as e:
            print(f"parse error: {e}")
            continue
        op, args = tokens[0], tokens[1:]

        if op in ("q", "quit", "exit"):
            break

        elif op in ("h", "?", "help"):
            print(_HELP)

        elif op == "i":
            _cmd_i(client, args)

        elif op == "r":
            _cmd_r(client, args)

        elif op == "t":
            _cmd_t(client, args)

        elif op == "d":
            _cmd_d(client, args)

        elif op == "s":
            _cmd_s(client, args)

        elif op == "w":
            if tailer.running() and (tailer.show_events or tailer.show_health):
                tailer.show_events = tailer.show_health = False
                if not tailer.show_snapshot:
                    tailer.stop()
                print("[watch off]")
            else:
                tailer.show_events = tailer.show_health = True
                tailer.start()
                print("[watch on — events + health]")

        elif op == "wt":
            if tailer.running() and tailer.show_snapshot:
                tailer.show_snapshot = False
                if not (tailer.show_events or tailer.show_health):
                    tailer.stop()
                print("[snapshot watch off]")
            else:
                tailer.show_snapshot = True
                tailer.start()
                print("[snapshot watch on]")

        elif op == "n":
            if not args:
                print("usage: n <host:port>")
                continue
            tailer.stop()
            client.close()
            target = args[0]
            client = Client(target)
            tailer = _Tailer(client)
            print(f"target → {target}")

        elif op == "ls":
            print(f"target: {target}  (tail running={tailer.running()})")

        else:
            print(f"unknown command '{op}' — h for help")

    tailer.stop()
    client.close()
    return 0

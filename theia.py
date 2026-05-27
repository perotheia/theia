#!/usr/bin/env python3
"""`theia` — the workspace command dispatcher.

Modeled on Mosaic's `moz` (up/mosaic-eng-ref/vehicle_os/mosaic.py): a flat
command map of name → external command, dispatched by argparse-style
matching. Self-contained — no dependencies beyond the stdlib, no click. The
`.art` / codegen surface stays on `artheia`; this wrapper is only for the
workspace-level lifecycle verbs that aren't artheia subcommands.

Commands:

    theia rig up          docker compose up   (the two-host deploy stack)
    theia rig down        docker compose down
    theia provision       puppet apply  — Phase 1 (install os pkgs + .ipk)
    theia orchestrate     puppet apply  — Phase 2 (app rollout, no restart)
    theia dist            bazel build the per-machine .ipk bundles
    theia install         bazel run //:install — local install into /install

Extra args after the verb pass through (e.g. `theia rig up central`,
`theia install --destdir /tmp/out`).
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

WORKSPACE = Path(__file__).resolve().parent
COMPOSE = WORKSPACE / "deploy" / "docker-compose.yml"
PUPPET = WORKSPACE / "deploy" / "puppet"
INSTALL_DESTDIR = "/install"


def _run(argv: list[str]) -> int:
    """Run a command from the workspace root, streaming output; return rc."""
    print(f"$ {' '.join(argv)}", file=sys.stderr)
    try:
        return subprocess.call(argv)
    except FileNotFoundError as e:
        print(f"theia: {e}", file=sys.stderr)
        return 127
    except KeyboardInterrupt:
        return 130


# --- commands: each takes pass-through args, returns an exit code -----------

def cmd_rig(args: list[str]) -> int:
    """docker compose up/down the deploy stack."""
    if not args or args[0] not in ("up", "down"):
        print("usage: theia rig {up|down} [compose-args...]", file=sys.stderr)
        return 2
    action, extra = args[0], list(args[1:])
    # `up` detaches by default so the shell returns; pass -d/--detach to keep.
    if action == "up" and not any(a in ("-d", "--detach") for a in extra):
        extra = ["-d", *extra]
    return _run(["docker", "compose", "-f", str(COMPOSE), action, *extra])


def _puppet(manifest: str, args: list[str]) -> int:
    return _run([
        "puppet", "apply",
        f"--modulepath={PUPPET / 'modules'}",
        str(PUPPET / manifest),
        "--hiera_config=/dev/null",
        *args,
    ])


def cmd_provision(args: list[str]) -> int:
    """puppet apply Phase 1 — install OS packages + .ipk artifacts.
    Reads /etc/theia/manifest/machine.yaml; target from $THEIA_MACHINE."""
    return _puppet("provisioning.pp", args)


def cmd_orchestrate(args: list[str]) -> int:
    """puppet apply Phase 2 — day-to-day app rollout, no supervisor restart."""
    return _puppet("orchestration.pp", args)


def cmd_dist(args: list[str]) -> int:
    """bazel build the per-machine .ipk deploy bundles. Pass machine image
    labels to narrow; default builds central + compute."""
    targets = args or [
        "@rig_demo//central_host:image",
        "@rig_demo//compute_host:image",
    ]
    return _run(["bazel", "build", *targets])


def cmd_install(args: list[str]) -> int:
    """bazel run //:install -- --destdir /install (local install). Override
    the destination by passing your own --destdir <dir>."""
    extra = list(args) if any(a == "--destdir" for a in args) \
        else ["--destdir", INSTALL_DESTDIR, *args]
    return _run(["bazel", "run", "//:install", "--", *extra])


COMMANDS = {
    "rig":         (cmd_rig,         "docker compose {up|down} the deploy stack"),
    "provision":   (cmd_provision,   "puppet apply — Phase 1 (os pkgs + .ipk)"),
    "orchestrate": (cmd_orchestrate, "puppet apply — Phase 2 (app rollout)"),
    "dist":        (cmd_dist,        "bazel build per-machine .ipk bundles"),
    "install":     (cmd_install,     f"bazel run //:install → {INSTALL_DESTDIR}"),
}


def _usage() -> None:
    print("usage: theia <command> [args...]\n\ncommands:", file=sys.stderr)
    for name, (_fn, desc) in COMMANDS.items():
        print(f"  {name:<12} {desc}", file=sys.stderr)
    print("\n(.art / codegen lives on `artheia` — run `artheia --help`.)",
          file=sys.stderr)


def main(argv: list[str]) -> int:
    if not argv or argv[0] in ("-h", "--help"):
        _usage()
        return 0 if argv else 2
    name, rest = argv[0], argv[1:]
    entry = COMMANDS.get(name)
    if entry is None:
        print(f"theia: unknown command {name!r}\n", file=sys.stderr)
        _usage()
        return 2
    fn, _desc = entry
    os.chdir(WORKSPACE)  # bazel / compose / puppet paths are workspace-relative
    return fn(rest)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

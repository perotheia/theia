#!/usr/bin/env python3
"""Workspace-level CLI dispatcher.

Modeled on Mosaic's `moz` (see up/mosaic-eng-ref/vehicle_os/mosaic.py)
but rewritten for our click-based artheia. Two kinds of subcommands:

1. **Artheia aliases** — `theia parse`, `theia gen-rig`, `theia
   executor emit`, etc. — forward verbatim to the `artheia` CLI.
   Mostly cosmetic (a shorter prefix) but with the benefit of one
   shared `--help` and uniform tab completion.

2. **Workspace-level verbs** that aren't artheia subcommands:
   - `theia run` — supervisor bringup recipe (emits executor.yaml,
     starts supervisor + com bridge + GUI, prints PIDs).
   - `theia repo` — passthrough to the `repo` CLI.
   - `theia bazel` — passthrough to `bazel`.

Shell completion:

    # bash:
    eval "$(_THEIA_COMPLETE=bash_source theia)"

    # zsh:
    eval "$(_THEIA_COMPLETE=zsh_source theia)"

    # fish:
    _THEIA_COMPLETE=fish_source theia | source

Click's built-in autocomplete drives this — no extra dependencies.
See docs/artheia/completion.md for the user-facing setup steps.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import click


# ---------------------------------------------------------------------------
# Workspace + tool discovery
# ---------------------------------------------------------------------------

WORKSPACE = Path(__file__).resolve().parent

# Prefer the venv-installed CLIs so `theia` works from any cwd without
# the user needing to source .venv/bin/activate first.
_VENV_BIN = WORKSPACE / ".venv" / "bin"


def _resolve(name: str) -> str:
    """Find a tool, preferring the workspace venv. Falls back to $PATH.
    Returns the absolute path or just the name (let exec resolve)."""
    in_venv = _VENV_BIN / name
    if in_venv.exists():
        return str(in_venv)
    found = shutil.which(name)
    return found if found else name


def _exec(argv: list[str]) -> "int | None":
    """Replace the current process with ``argv`` (os.execvp). Returns
    only if exec fails — callers shouldn't see the return value in
    the happy path."""
    try:
        os.execvp(argv[0], argv)
    except FileNotFoundError as e:
        click.secho(f"error: {e}", fg="red", err=True)
        sys.exit(127)


# ---------------------------------------------------------------------------
# Top-level group
# ---------------------------------------------------------------------------

@click.group(
    context_settings={
        "help_option_names": ["-h", "--help"],
        "ignore_unknown_options": True,
    },
    help=(
        "Theia workspace CLI. Wraps artheia and a few workspace-level "
        "operations. Run `theia <subcommand> --help` for details."
    ),
)
def cli() -> None:
    """Top-level group; click dispatches into subcommands."""


# ---------------------------------------------------------------------------
# Artheia passthroughs
# ---------------------------------------------------------------------------
#
# Each command forwards stdin/stdout/stderr to `artheia <verb>` via
# os.execvp — preserves exit codes, signal handling, and lets click's
# own completion machinery dispatch into artheia's own (transparent
# completion through the alias).

_ARTHEIA = _resolve("artheia")

# The list of artheia verbs that get aliased — keep in sync with
# `artheia --help`. Subcommand groups (executor, gui) get their own
# click.group below so users can `theia executor emit ...` rather than
# `theia executor -- emit ...`.
_ARTHEIA_VERBS = [
    "parse",
    "gen-app",
    "gen-app-composition",
    "gen-app-dispatch",
    "gen-autosar-system",
    "gen-can-codec",
    "gen-codec-dispatch",
    "gen-cpp-stubs",
    "gen-etcd",
    "gen-fibex-codec",
    "gen-gw-types",
    "gen-host-netgraph",
    "gen-netgraph",
    "gen-netgraph-partition",
    "gen-platform-protos",
    "gen-proto",
    "gen-proto-package",
    "gen-psp-registry",
    "gen-rig",
    "gen-routing",
    "gen-signal-filter",
    "generate-manifest",
    "import-dbc",
    "import-fibex",
    "signal-filter",
]


def _make_passthrough(verb: str):
    """Build a click command that execs `artheia <verb> <args>`."""

    @click.command(
        name=verb,
        help=f"Passthrough to `artheia {verb}`. Run `theia {verb} --help` for flags.",
        context_settings={
            "ignore_unknown_options": True,
            "allow_extra_args": True,
            "help_option_names": [],  # Let artheia render its own --help.
        },
    )
    @click.argument("args", nargs=-1, type=click.UNPROCESSED)
    def _cmd(args: tuple[str, ...]) -> None:
        _exec([_ARTHEIA, verb, *args])

    return _cmd


for _verb in _ARTHEIA_VERBS:
    cli.add_command(_make_passthrough(_verb))


# ---------------------------------------------------------------------------
# Artheia subcommand groups — `executor emit`, `gui emit`
# ---------------------------------------------------------------------------
#
# These get their own group so `theia executor emit ...` works
# transparently. We don't need to enumerate inner verbs — the inner
# UNPROCESSED args run through to artheia.

@cli.group(
    "executor",
    context_settings={"ignore_unknown_options": True, "help_option_names": []},
    help="Erlang-style executor commands (passthrough to `artheia executor`).",
)
def executor() -> None:
    pass


@executor.command(
    "emit",
    context_settings={"ignore_unknown_options": True, "allow_extra_args": True,
                      "help_option_names": []},
    help="Emit executor.yaml. See `theia executor emit --help` for flags.",
)
@click.argument("args", nargs=-1, type=click.UNPROCESSED)
def _exec_emit(args: tuple[str, ...]) -> None:
    _exec([_ARTHEIA, "executor", "emit", *args])


@cli.group(
    "gui",
    context_settings={"ignore_unknown_options": True, "help_option_names": []},
    help="Supervisor-GUI manifest commands (passthrough to `artheia gui`).",
)
def gui_grp() -> None:
    pass


@gui_grp.command(
    "emit",
    context_settings={"ignore_unknown_options": True, "allow_extra_args": True,
                      "help_option_names": []},
    help="Emit machines.yaml. See `theia gui emit --help` for flags.",
)
@click.argument("args", nargs=-1, type=click.UNPROCESSED)
def _gui_emit(args: tuple[str, ...]) -> None:
    _exec([_ARTHEIA, "gui", "emit", *args])


# ---------------------------------------------------------------------------
# Workspace-level passthroughs
# ---------------------------------------------------------------------------

@cli.command(
    "bazel",
    context_settings={"ignore_unknown_options": True, "allow_extra_args": True,
                      "help_option_names": []},
    help="Passthrough to bazel. Bazel must be on $PATH (or in the venv).",
)
@click.argument("args", nargs=-1, type=click.UNPROCESSED)
def _bazel(args: tuple[str, ...]) -> None:
    bazel = _resolve("bazel")
    _exec([bazel, *args])


@cli.command(
    "repo",
    context_settings={"ignore_unknown_options": True, "allow_extra_args": True,
                      "help_option_names": []},
    help="Passthrough to the `repo` (google-repo) CLI.",
)
@click.argument("args", nargs=-1, type=click.UNPROCESSED)
def _repo(args: tuple[str, ...]) -> None:
    repo = _resolve("repo")
    _exec([repo, *args])


# ---------------------------------------------------------------------------
# `theia run` — supervisor bringup recipe
# ---------------------------------------------------------------------------

@cli.command(
    "run",
    help=(
        "Bring up a rig: emit executor.yaml, start the supervisor, the "
        "services/com gRPC bridge, and optionally the GUI. Tears down "
        "on Ctrl-C."
    ),
)
@click.argument("rig_module")
@click.option(
    "--rig",
    "rig_attr",
    default=None,
    help="Rig/SoftwareSpecification attribute name (default: auto-pick).",
)
@click.option(
    "--root-dir",
    type=click.Path(exists=True, file_okay=False),
    default=None,
    help="Runtime root directory (default: ../theia_runtime).",
)
@click.option(
    "--no-gui",
    is_flag=True,
    help="Skip launching supervisor-gui.",
)
@click.option(
    "--executor-yaml",
    "executor_yaml",
    type=click.Path(dir_okay=False),
    default="/tmp/theia_executor.yaml",
    help="Where to write the emitted executor.yaml.",
)
@click.option(
    "--machines-yaml",
    "machines_yaml",
    type=click.Path(dir_okay=False),
    default="/tmp/theia_machines.yaml",
    help="Where to write the emitted machines.yaml.",
)
def _run(
    rig_module: str,
    rig_attr: "str | None",
    root_dir: "str | None",
    no_gui: bool,
    executor_yaml: str,
    machines_yaml: str,
) -> None:
    """Bringup orchestration: emit + supervisor + com bridge + GUI."""

    # Default runtime root: ../theia_runtime relative to workspace.
    if root_dir is None:
        default_runtime = WORKSPACE.parent / "theia_runtime"
        if default_runtime.is_dir():
            root_dir = str(default_runtime)
        else:
            click.secho(
                f"error: --root-dir not set and default "
                f"{default_runtime} does not exist",
                fg="red", err=True,
            )
            sys.exit(2)

    # Step 1: emit the executor.yaml.
    emit_cmd = [_ARTHEIA, "executor", "emit", rig_module, "--out", executor_yaml]
    if rig_attr:
        emit_cmd += ["--rig", rig_attr]
    click.secho(f"$ {' '.join(emit_cmd)}", fg="cyan")
    rc = subprocess.call(emit_cmd)
    if rc != 0:
        sys.exit(rc)

    # Step 2: emit the machines.yaml (GUI manifest).
    gui_emit_cmd = [_ARTHEIA, "gui", "emit", rig_module, "--out", machines_yaml]
    if rig_attr:
        gui_emit_cmd += ["--rig", rig_attr]
    click.secho(f"$ {' '.join(gui_emit_cmd)}", fg="cyan")
    rc = subprocess.call(gui_emit_cmd)
    if rc != 0:
        sys.exit(rc)

    # Step 3: locate the binaries (built under platform/supervisor/build/ etc).
    supervisor_bin = WORKSPACE / "platform" / "supervisor" / "build" / "supervisor"
    com_bin = WORKSPACE / "services" / "com" / "build" / "services-com"
    gui_bin = WORKSPACE / "supervisor-gui" / "build" / "supervisor-gui"

    for name, path in [
        ("supervisor", supervisor_bin),
        ("services-com", com_bin),
    ]:
        if not path.exists():
            click.secho(
                f"error: {name} binary not found at {path}; "
                f"build it first (`cd {path.parent.parent} && cmake --build build`)",
                fg="red", err=True,
            )
            sys.exit(2)

    procs: list[tuple[str, subprocess.Popen]] = []

    def _launch(name: str, argv: list[str], log: str) -> None:
        click.secho(f"$ {' '.join(argv)} >{log}", fg="cyan")
        log_fh = open(log, "w")
        proc = subprocess.Popen(argv, stdout=log_fh, stderr=subprocess.STDOUT)
        procs.append((name, proc))

    try:
        _launch(
            "supervisor",
            [str(supervisor_bin), "run", executor_yaml, "--root-dir", root_dir],
            "/tmp/theia_supervisor.log",
        )
        _launch(
            "services-com",
            [str(com_bin), "--listen", "0.0.0.0:7700"],
            "/tmp/theia_com.log",
        )

        if not no_gui:
            if not gui_bin.exists():
                click.secho(
                    f"warning: supervisor-gui not built at {gui_bin}; skipping",
                    fg="yellow", err=True,
                )
            else:
                _launch(
                    "supervisor-gui",
                    [str(gui_bin), "-m", machines_yaml],
                    "/tmp/theia_gui.log",
                )

        click.echo()
        click.secho("=== running ===", bold=True)
        for name, proc in procs:
            click.echo(f"  {name:<15} pid={proc.pid}  log=/tmp/theia_{name.replace('-', '_')}.log")
        click.echo("\nPress Ctrl-C to tear down.")

        # Block until any child dies or the user hits Ctrl-C.
        try:
            while procs:
                for name, proc in list(procs):
                    rc = proc.poll()
                    if rc is not None:
                        click.secho(
                            f"\n[{name}] exited with rc={rc}; tearing down",
                            fg="yellow",
                        )
                        raise KeyboardInterrupt
                import time
                time.sleep(0.5)
        except KeyboardInterrupt:
            pass

    finally:
        for name, proc in reversed(procs):
            if proc.poll() is None:
                click.secho(f"  terminating {name} (pid={proc.pid})", fg="cyan")
                proc.terminate()
        for name, proc in reversed(procs):
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                click.secho(f"  killing {name} (pid={proc.pid})", fg="yellow")
                proc.kill()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    cli()

#!/usr/bin/env python3
"""theia MCP server — the ``theia`` dev-loop CLI + the tdb/rtdb inspect bridges
exposed as MCP tools, so a coding agent can drive the full tutorial dev-flow
without a shell.

Design (see the MCP-tooling audit): artheia already covers *model/codegen* and
rf-theia covers *test*. The gap this server fills is the middle + deploy edge of
the flow:

  - local dev loop  : init / manifest / install / start / stop / cast / call
  - live inspect     : tdb apps|ps|trace  (+ rtdb machines|ps for remote boards)
  - fleet deploy     : colony web API  (POST /deployments, status, log, rigs)

DELIBERATELY OUT OF SCOPE (kept shell-only, human-gated): release / release-swp
/ release-role (the semver + migration-approval gate is a human checkpoint),
dist / clean / compdb / observer (one-shot ceremony, no branch-on-output). An
agent PROPOSES a release; a human runs it. See docs — the "find balance" call.

Invocation: theia.py's ``main(argv)`` and tdb/rtdb's ``main(argv)`` are imported
and called IN-PROCESS (like artheia's CliRunner), honoring THEIA_INVOCATION_CWD
for workspace resolution. ``theia start`` daemonizes its supervisor via a
double-fork inside cmd_start, so it returns promptly here; we never hold a
long-lived child.
"""

from __future__ import annotations

import contextlib
import io
import os
import shlex
import subprocess
import sys
from pathlib import Path

import httpx
from fastmcp import FastMCP

# tools/ is on sys.path when run as `python -m` from tools/'s parent; make
# theia.py importable regardless of how run_mcp.sh launches us.
_TOOLS_DIR = Path(__file__).resolve().parent
if str(_TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(_TOOLS_DIR))

# theia.py is a clean argparse module with main(argv) — imported and run
# in-process. tdb/rtdb are single-file scripts that sys.path-insert their own
# dir and import sibling modules under BARE names (`tdb_commands`); importing
# both into ONE process collides on those names, so we run them as short-lived
# subprocesses instead (they daemonize nothing — pure reads).
import theia as _theia          # tools/theia.py → main(argv) -> int

_TDB_PY = _TOOLS_DIR / "tdb" / "tdb.py"
_RTDB_PY = _TOOLS_DIR / "rtdb" / "rtdb.py"


mcp = FastMCP(
    "theia",
    instructions=(
        "The `theia` dev-loop CLI + tdb/rtdb live-inspect bridges as an API. "
        "Drives the local build/run/verify loop (init, manifest, install, "
        "start, stop, cast, call), inspects the running supervisor tree "
        "(tdb_apps/tdb_ps/tdb_trace, and rtdb_* for a remote board over com), "
        "and deploys to the fleet via the colony web API (colony_deploy + "
        "status/log). Release/publish verbs are intentionally NOT here — they "
        "are human-gated. Paths resolve against the workspace root."
    ),
)

# Workspace root — same contract as the artheia/rf-theia servers.
_WORKSPACE = Path(os.environ.get("THEIA_INVOCATION_CWD") or os.getcwd()).resolve()

# colony web API base + optional mutating-route key (X-Colony-Key).
_COLONY_URL = os.environ.get("COLONY_API_URL", "http://dalek:8081").rstrip("/")
_COLONY_KEY = os.environ.get("COLONY_API_KEY", "")


def _theia_run(argv: list[str]) -> str:
    """Invoke theia.py's ``main(argv) -> int`` in-process from the workspace
    root, capturing combined stdout+stderr and the exit code. theia's own main()
    chdirs to the checkout but reads the workspace from THEIA_INVOCATION_CWD
    (which run_mcp.sh sets)."""
    os.environ.setdefault("THEIA_INVOCATION_CWD", str(_WORKSPACE))
    out, err = io.StringIO(), io.StringIO()
    prev = os.getcwd()
    rc: int | str
    try:
        os.chdir(_WORKSPACE)
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = _theia.main(argv)
    except SystemExit as e:               # argparse error() / --help path
        rc = e.code if e.code is not None else 0
    except Exception as e:                # surface, don't crash the server
        rc = f"EXC {type(e).__name__}: {e}"
    finally:
        os.chdir(prev)
    o, e = out.getvalue(), err.getvalue()
    head = f"$ theia {' '.join(shlex.quote(a) for a in argv)}\n"
    body = o + (("\n" + e) if e.strip() and e.strip() not in o else "")
    return f"{head}[exit {rc}]\n{body}".rstrip() + "\n"


def _dbg_run(script: Path, prog: str, argv: list[str],
             timeout: float = 90.0) -> str:
    """Run tdb/rtdb as a short-lived subprocess (same interpreter, from the
    workspace root) and return combined output + exit code. They're pure reads
    of a live supervisor / com bridge, so a subprocess is clean and safe."""
    os.environ.setdefault("THEIA_INVOCATION_CWD", str(_WORKSPACE))
    try:
        p = subprocess.run(
            [sys.executable, str(script), *argv],
            cwd=str(_WORKSPACE), capture_output=True, text=True,
            timeout=timeout, env=os.environ.copy(),
        )
        rc: int | str = p.returncode
        body = p.stdout + (("\n" + p.stderr)
                           if p.stderr.strip() and p.stderr.strip() not in p.stdout
                           else "")
    except subprocess.TimeoutExpired as e:
        rc = "TIMEOUT"
        def _s(x: object) -> str:
            return x.decode(errors="replace") if isinstance(x, bytes) else (x or "")  # type: ignore[arg-type]
        so, se = _s(e.stdout), _s(e.stderr)
        body = so + (("\n" + se) if se else "")
    head = f"$ {prog} {' '.join(shlex.quote(a) for a in argv)}\n"
    return f"{head}[exit {rc}]\n{body}".rstrip() + "\n"


# ── local dev loop ──────────────────────────────────────────────────────────

@mcp.tool()
def theia_init(with_services: bool = False) -> str:
    """Scaffold a Theia workspace in the current directory (the workspace root).
    Pass with_services=True to include the platform services rig. Idempotent."""
    argv = ["init"] + (["--with-services"] if with_services else [])
    return _theia_run(argv)


@mcp.tool()
def theia_manifest(target: str, attr: str | None = None) -> str:
    """Serialize a rig (Python `rig.py` module) into per-machine JSON manifests
    under dist/manifest/. `target` is the rig module (e.g. 'rig', 'test_rig').
    `attr` selects one *Software export when a module has several (e.g. 'DOCKER').
    The error surface here (TIPC address collisions, role/name invariants) is
    the signal to read before install."""
    argv = ["manifest", target] + (["--attr", attr] if attr else [])
    return _theia_run(argv)


@mcp.tool()
def theia_install(target: str, machine: str | None = None) -> str:
    """Serialize the manifest, bazel-build the FCs, and stage supervisor + child
    binaries into install/ for the local machine (or `machine` in a multi-machine
    rig). This is the build step — read its output for compile errors."""
    argv = ["install", target] + (["--machine", machine] if machine else [])
    return _theia_run(argv)


@mcp.tool()
def theia_start(instance: int | None = None) -> str:
    """Start the local supervisor daemon (from install/). It double-forks and
    detaches, so this returns once the daemon is up. `instance` starts a second
    supervisor instance for a multi-instance local rig. Verify with tdb_apps."""
    argv = ["start"] + (["--instance", str(instance)] if instance is not None else [])
    return _theia_run(argv)


@mcp.tool()
def theia_stop() -> str:
    """Stop the local supervisor daemon (and its supervised tree)."""
    return _theia_run(["stop"])


@mcp.tool()
def theia_cast(node: str, msg_type: str, data: str = "{}",
               instance: int | None = None, machine: int | None = None) -> str:
    """Fire-and-forget a message at a live node (async cast). `data` is a JSON
    object matching the message fields; it is packed to proto and sent over TIPC
    via artheia.probe. `instance`/`machine` target one clone in a multiplicity
    rig. The fastest inner-loop verify — poke a node, then read tdb_trace."""
    argv = ["cast", node, msg_type, "--data", data]
    if instance is not None:
        argv += ["--instance", str(instance)]
    if machine is not None:
        argv += ["--machine", str(machine)]
    return _theia_run(argv)


@mcp.tool()
def theia_call(node: str, op: str, data: str = "{}",
               instance: int | None = None, machine: int | None = None) -> str:
    """Synchronous request/reply against a live node (call). Same JSON→proto→TIPC
    path as cast, but blocks for and returns the reply. Use to read a node's
    state or invoke a control op. `instance`/`machine` target one clone."""
    argv = ["call", node, op, "--data", data]
    if instance is not None:
        argv += ["--instance", str(instance)]
    if machine is not None:
        argv += ["--machine", str(machine)]
    return _theia_run(argv)


# ── live inspect (tdb = local TIPC ns; rtdb = remote board over com) ─────────

@mcp.tool()
def tdb_apps() -> str:
    """The supervised tree of the LOCAL supervisor: supervisor → services → your
    FC nodes, with live state. Use after install+start to confirm nodes are up
    (and after a deploy to confirm the SWP landed)."""
    return _dbg_run(_TDB_PY, "tdb", ["apps"])


@mcp.tool()
def tdb_ps() -> str:
    """Flat Linux-ps view of the local system: one row per worker (PID/TID/name/
    CPU/mem/threads). The process-level counterpart to tdb_apps' tree."""
    return _dbg_run(_TDB_PY, "tdb", ["ps"])


@mcp.tool()
def tdb_trace(count: int = 50) -> str:
    """Follow the live trace firehose (log[trace]) and return the next `count`
    decoded records as JSON, then stop. This is how you observe a node's actual
    behaviour (handle_cast ticks, GenStateM transitions) after a cast/call —
    bounded so it returns instead of tailing forever."""
    return _dbg_run(_TDB_PY, "tdb",
                    ["tracecat", "--json", "--count", str(count)])


@mcp.tool()
def rtdb_machines() -> str:
    """List the machines reachable through the `com` gRPC bridge (the remote
    fleet view). Use to pick a --machine for rtdb_ps after a deploy."""
    return _dbg_run(_RTDB_PY, "rtdb", ["machines"])


@mcp.tool()
def rtdb_ps(machine: str | None = None) -> str:
    """Flat process view of a REMOTE board over com (rtdb = tdb across the
    network). `machine` selects one board by name; omit for the default."""
    argv = ["ps"] + ([machine] if machine else [])
    return _dbg_run(_RTDB_PY, "rtdb", argv)


# ── fleet deploy via the colony web API ──────────────────────────────────────

def _colony(method: str, path: str, *, json_body: dict | None = None) -> str:
    """Call the colony web API. Mutating routes need X-Colony-Key when the
    server was started with COLONY_API_KEY set."""
    url = f"{_COLONY_URL}{path}"
    headers = {"X-Colony-Key": _COLONY_KEY} if _COLONY_KEY else {}
    try:
        with httpx.Client(timeout=30.0) as c:
            r = c.request(method, url, json=json_body, headers=headers)
        return (f"{method} {url}\n[{r.status_code}]\n"
                + (r.text.strip() or "(empty)") + "\n")
    except httpx.HTTPError as e:
        return f"{method} {url}\n[ERROR] {type(e).__name__}: {e}\n"


@mcp.tool()
def colony_rigs() -> str:
    """List the colony deploy targets (≈ Mender devices / registry rigs). Pick a
    `rig` from here for colony_deploy."""
    return _colony("GET", "/rigs")


@mcp.tool()
def colony_deploy(rig: str, kind: str = "orchestrate",
                  host: str | None = None, name: str | None = None) -> str:
    """Enqueue a colony deploy play against `rig`. kind ∈ {provision, orchestrate,
    cleanup}: `provision` lays the base runtime, `orchestrate` (re)deploys the
    configured stack, `cleanup` tears it down. Pass `host` (ip[:port]) for a
    registry-free per-device deploy. Returns the deployment id — poll it with
    colony_deployment_status."""
    body: dict = {"rig": rig, "kind": kind}
    if host:
        body["host"] = host
    if name:
        body["name"] = name
    return _colony("POST", "/deployments", json_body=body)


@mcp.tool()
def colony_deployment_status(deployment_id: str) -> str:
    """Get one deployment's status (active|scheduled|finished + result). Poll
    after colony_deploy to know when the rollout has landed."""
    return _colony("GET", f"/deployments/{deployment_id}")


@mcp.tool()
def colony_deployment_log(deployment_id: str) -> str:
    """Get the Ansible run log for a deployment — the detail behind a failed or
    in-flight rollout."""
    return _colony("GET", f"/deployments/{deployment_id}/log")


def main() -> None:
    mcp.run(show_banner=False)


if __name__ == "__main__":
    main()

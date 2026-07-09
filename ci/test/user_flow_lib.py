"""Robot library for the end-user-flow harness (ci/run.sh) — RUNTIME checks.

Thin, workspace-agnostic keywords over the same surfaces a user touches:
`theia call` (the request/reply path through a live node), the TIPC nametable
(is a node bound?), and `tdb ps` (is the supervised tree up?). The heavy
orchestration (init/gen/build/install/start) lives in ci/run.sh — Robot asserts
only the RUNTIME behaviour.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

from robot.api import logger
from robot.api.deco import keyword, library

_THEIA_ROOT = Path(__file__).resolve().parents[2]
_THEIA = ["python3", str(_THEIA_ROOT / "tools" / "theia.py")]


def _run(cmd: list[str], cwd: str | None = None, timeout: int = 30) -> str:
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                       timeout=timeout)
    if p.returncode != 0:
        raise AssertionError(
            f"{' '.join(cmd)} failed (rc={p.returncode}):\n{p.stdout}\n{p.stderr}")
    return p.stdout


@library(scope="SUITE")
class user_flow_lib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    @keyword("Theia Call")
    def theia_call(self, ws: str, node: str, op: str, data: str = "{}") -> dict:
        """`theia call <node> <op> --data <json>` in workspace *ws*; returns the
        parsed reply dict (the last JSON line of the output)."""
        out = _run(_THEIA + ["call", node, op, "--data", data], cwd=ws)
        # the reply is the JSON object at the END of the output (pretty-printed
        # over multiple lines for non-empty replies) — join from its "{" line.
        lines = out.strip().splitlines()
        for i, line in enumerate(lines):
            if line.strip().startswith("{"):
                rep = json.loads("\n".join(lines[i:]))
                logger.info(f"{node}.{op}({data}) -> {rep}")
                return rep
        raise AssertionError(f"no JSON reply in output:\n{out}")

    @keyword("Tipc Bound")
    def tipc_bound(self, hex_type: str) -> None:
        """Assert a TIPC service type (e.g. 0xD0010001) is bound."""
        if not self._bound(hex_type):
            raise AssertionError(f"TIPC {hex_type} NOT bound")

    @keyword("Tipc Not Bound")
    def tipc_not_bound(self, hex_type: str) -> None:
        if self._bound(hex_type):
            raise AssertionError(f"TIPC {hex_type} unexpectedly bound")

    @keyword("Supervised Process Count")
    def process_count(self, ws: str) -> int:
        """Rows in `tdb ps` (excluding the header) for the workspace's stack."""
        env = dict(os.environ, THEIA_WORKSPACE=ws)
        # invoke tdb by repo path — CI has no .venv symlink shim on PATH.
        tdb = [sys.executable, str(_THEIA_ROOT / "tools" / "tdb" / "tdb.py")]
        p = subprocess.run(tdb + ["ps"], cwd=ws, capture_output=True,
                           text=True, timeout=30, env=env)
        rows = [l for l in p.stdout.splitlines()
                if l.strip() and not l.lstrip().startswith("PID")]
        logger.info(f"tdb ps rows: {len(rows)}\n{p.stdout}")
        return len(rows)

    @staticmethod
    def _bound(hex_type: str) -> bool:
        want = int(hex_type, 16)
        p = subprocess.run(["tipc", "nametable", "show"],
                           capture_output=True, text=True, timeout=10)
        for line in p.stdout.splitlines():
            cols = line.split()
            if cols and cols[0].isdigit() and int(cols[0]) == want:
                return True
        return False

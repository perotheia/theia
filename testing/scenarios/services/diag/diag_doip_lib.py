"""Robot library wrapping the diag DoIP/UDS e2e smoke for uniform reporting.

The Python smoke (services/diag/test/diag_doip_smoke.py) is the single source of
truth for WHAT is checked: it drives the live diag FC over REAL DoIP (TCP/13400)
— routing activation + the UDS v1 set (0x10 session, 0x22 identity + fault-log
DIDs, 0x2E write, 0x19 read DTC) — and asserts the responses. This library only
RUNS it + a couple of focused keywords and surfaces pass/fail into Robot's
output.xml / report.html so a diag regression reports alongside every other
scenario. Useful as the diag e2e gate.

Needs the stack up (`theia start`, so the supervisor forks diag → DoipServer
binds 13400). The keyword SKIPS cleanly when diag isn't bound, so a hermetic CI
lane doesn't false-fail.
"""
from __future__ import annotations

import socket
import subprocess
import sys
from pathlib import Path

from robot.api import logger
from robot.api.deco import keyword, library

# .../testing/scenarios/services/diag/diag_doip_lib.py → repo root:
# diag[0] services[1] scenarios[2] testing[3] theia[4].
_WS = Path(__file__).resolve().parents[4]
_SMOKE = _WS / "services" / "diag" / "test" / "diag_doip_smoke.py"
# diag's DoipServer TIPC service type (system.services.diag = 0x80010017).
_DIAG_TIPC_DEC = 0x80010017
_DOIP_PORT = 13400


@library(scope="SUITE")
class DiagDoipLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    @keyword("Require Diag Listening")
    def require_diag_listening(self) -> None:
        """Skip the suite unless diag is up — DoipServer bound on TIPC AND the
        DoIP port accepting. Keeps the test honest in a hermetic lane: no live
        diag → SKIP, not FAIL."""
        # (a) TIPC binding (the FC is forked + bound).
        bound = False
        try:
            out = subprocess.run(
                ["tipc", "nametable", "show"],
                capture_output=True, text=True, timeout=10,
            ).stdout
            bound = str(_DIAG_TIPC_DEC) in out
        except (FileNotFoundError, subprocess.SubprocessError):
            logger.info("`tipc` unavailable — falling back to the port check.")
            bound = True   # defer to the port probe below
        # (b) the DoIP TCP port is accepting.
        port_open = False
        try:
            with socket.create_connection(("127.0.0.1", _DOIP_PORT), timeout=2):
                port_open = True
        except OSError:
            port_open = False
        if not (bound and port_open):
            from robot.api import SkipExecution
            raise SkipExecution(
                f"diag not ready (TIPC {hex(_DIAG_TIPC_DEC)} bound={bound}, "
                f"DoIP :{_DOIP_PORT} open={port_open}) — start the stack with "
                "`theia start`.")

    @keyword("Run Diag DoIP Smoke")
    def run_diag_doip_smoke(self) -> None:
        """Run diag_doip_smoke.py against the live diag FC over DoIP/13400;
        FAIL on non-zero exit. The script's stdout (VIN / fault count / DTC
        verdict) is logged so a failure is self-explanatory in report.html."""
        if not _SMOKE.is_file():
            raise AssertionError(f"smoke test not found at {_SMOKE}")
        py = _WS / ".venv" / "bin" / "python"
        proc = subprocess.run(
            [str(py if py.exists() else sys.executable), str(_SMOKE)],
            cwd=str(_WS), capture_output=True, text=True, timeout=30,
        )
        logger.info(f"--- diag_doip_smoke.py stdout ---\n{proc.stdout}")
        if proc.stderr.strip():
            logger.info(f"--- stderr ---\n{proc.stderr}")
        if proc.returncode != 0:
            raise AssertionError(
                f"diag DoIP smoke FAILED (exit {proc.returncode}):\n"
                f"{proc.stdout}\n{proc.stderr}")
        logger.info("diag DoIP smoke PASSED.")

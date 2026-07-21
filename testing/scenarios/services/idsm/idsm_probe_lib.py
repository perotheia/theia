"""Robot library wrapping the IDSM probe smoke test for consistent reporting.

The Python smoke test (services/idsm/test/idsm_probe_smoke.py) stays the single
source of truth for WHAT is checked: it drives the live idsm FC via the tdb
TdbIdsm client and asserts GetIdsStatus serves a definite IdsState over TIPC
(UNAVAILABLE = the honest graceful-degrade where no eBPF backend exists). This
library only RUNS it and surfaces pass/fail into Robot's output.xml / report.html
so an idsm regression reports alongside every other scenario, rather than as a
loose script. (It is fine that Robot doesn't drive the probe itself — the value
here is uniform reporting, not re-implementing the probe in keywords.)

Needs the stack up (`theia start -d`): the keyword detects idsm's TIPC binding and
SKIPS cleanly when no stack is running, so a hermetic CI lane doesn't false-fail.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

from robot.api import logger
from robot.api.deco import keyword, library

# .../testing/scenarios/services/idsm/idsm_probe_lib.py → repo root:
# idsm[0] services[1] scenarios[2] testing[3] theia[4].
_WS = Path(__file__).resolve().parents[4]
_SMOKE = _WS / "services" / "idsm" / "test" / "idsm_probe_smoke.py"
# idsm's TIPC service type (system.services.idsm IdsmDaemon = 0x8001000F). The
# nametable encodes the service id directly; 0x8001000F = 2147549199.
_IDSM_TIPC_DEC = 0x8001000F


@library(scope="SUITE")
class IdsmProbeLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    @keyword("Require Idsm Listening")
    def require_idsm_listening(self) -> None:
        """Skip the suite unless idsm is bound on TIPC (the stack is up).

        Keeps the test honest in a hermetic lane: no live stack → SKIP, not
        FAIL. Uses `tipc nametable show` (read-only); if `tipc` is missing we
        let the smoke test itself be the gate (it ConnectionErrors fast)."""
        try:
            out = subprocess.run(
                ["tipc", "nametable", "show"],
                capture_output=True, text=True, timeout=10,
            ).stdout
        except (FileNotFoundError, subprocess.SubprocessError):
            logger.info("`tipc` unavailable — deferring to the smoke test's "
                        "own connect gate.")
            return
        if str(_IDSM_TIPC_DEC) not in out:
            from robot.api import SkipExecution
            raise SkipExecution(
                f"idsm not bound (TIPC type {hex(_IDSM_TIPC_DEC)} absent from "
                "the nametable) — start the stack with `theia start -d`.")

    @keyword("Run Idsm Probe Smoke")
    def run_idsm_probe_smoke(self) -> None:
        """Run idsm_probe_smoke.py against the live stack; FAIL on non-zero exit.

        The script's stdout (the GetIdsStatus snapshot + the smoke verdict) is
        logged so a failure is self-explanatory in report.html."""
        if not _SMOKE.is_file():
            raise AssertionError(f"smoke test not found at {_SMOKE}")
        env = {"PATH": f"{_WS / '.venv' / 'bin'}:/usr/bin:/bin"}
        # Mirror how every other tool runs: venv python, repo as cwd.
        proc = subprocess.run(
            [str(_WS / ".venv" / "bin" / "python"), str(_SMOKE)],
            cwd=str(_WS), env=env, capture_output=True, text=True, timeout=30,
        )
        logger.info(f"--- idsm_probe_smoke.py stdout ---\n{proc.stdout}")
        if proc.stderr.strip():
            logger.info(f"--- stderr ---\n{proc.stderr}")
        if proc.returncode != 0:
            raise AssertionError(
                f"idsm probe smoke FAILED (exit {proc.returncode}):\n"
                f"{proc.stdout}\n{proc.stderr}")
        logger.info("idsm probe smoke PASSED.")

"""Robot library wrapping the cooperative-alert consensus probe suite (HANDOFF2).

The Python suite (packages/v2v/test/consensus_probe.py) is the single source of
truth for WHAT is checked. It has two layers:

  - a HERMETIC multi-node sim (E1-E7 parity, `--sim-only`) — the algorithm gate,
    the SAME math the C++ AlertConsensus implements; runs offline in CI.
  - a LIVE-FC check — inject AlertBelief-bearing beacons into the running osi FC
    via the tdb TdbV2v client (replacing the Meshtastic radio, which the deploy
    disables with run_on_start=false) and assert GetAlertDecision converges.

This library RUNS each layer and surfaces pass/fail into Robot's output so a
consensus regression reports alongside every other scenario.
"""
from __future__ import annotations

import subprocess
from pathlib import Path

from robot.api import logger
from robot.api.deco import keyword, library

# .../testing/scenarios/services/osi/consensus_probe_lib.py → repo root:
# osi[0] services[1] scenarios[2] testing[3] theia[4].
_WS = Path(__file__).resolve().parents[4]
_SUITE = _WS / "packages" / "v2v" / "test" / "consensus_probe.py"
# osi OsiV2v TIPC service type (system.services.osi OsiV2v = 0x800100A0).
_OSI_V2V_TIPC_DEC = 0x800100A0


def _py() -> str:
    venv = _WS / ".venv" / "bin" / "python"
    return str(venv) if venv.is_file() else "python3"


@library(scope="SUITE")
class ConsensusProbeLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    @keyword("Run Consensus Parity Sim")
    def run_consensus_parity_sim(self) -> None:
        """Run the HERMETIC E1-E7 parity sim (`--sim-only`). No FC, no radios —
        the algorithm gate. FAIL on non-zero exit."""
        if not _SUITE.is_file():
            raise AssertionError(f"consensus probe not found at {_SUITE}")
        proc = subprocess.run(
            [_py(), str(_SUITE), "--sim-only"],
            cwd=str(_WS), capture_output=True, text=True, timeout=120,
        )
        logger.info(f"--- consensus_probe.py --sim-only ---\n{proc.stdout}")
        if proc.stderr.strip():
            logger.info(f"--- stderr ---\n{proc.stderr}")
        if proc.returncode != 0:
            raise AssertionError(
                f"consensus parity sim FAILED (exit {proc.returncode}):\n"
                f"{proc.stdout}\n{proc.stderr}")
        logger.info("consensus parity (E1-E7) PASSED.")

    @keyword("Require Osi V2v Listening")
    def require_osi_v2v_listening(self) -> None:
        """Skip the LIVE case unless OsiV2v is bound on TIPC (the stack is up)."""
        try:
            out = subprocess.run(
                ["tipc", "nametable", "show"],
                capture_output=True, text=True, timeout=10,
            ).stdout
        except (FileNotFoundError, subprocess.SubprocessError):
            logger.info("`tipc` unavailable — deferring to the probe's own gate.")
            return
        if str(_OSI_V2V_TIPC_DEC) not in out:
            from robot.api import SkipExecution
            raise SkipExecution(
                f"OsiV2v not bound (TIPC type {hex(_OSI_V2V_TIPC_DEC)} absent) — "
                "start the stack with the meshtastic node run_on_start=false.")

    @keyword("Run Consensus Live Probe")
    def run_consensus_live_probe(self) -> None:
        """Run the LIVE-FC check: the probe injects beacons over TIPC (replacing
        the radio) and asserts the FC converges. `--live` makes an absent FC a
        FAIL (the Require keyword already SKIPs when the stack is down)."""
        proc = subprocess.run(
            [_py(), str(_SUITE), "--live"],
            cwd=str(_WS), capture_output=True, text=True, timeout=120,
        )
        logger.info(f"--- consensus_probe.py --live ---\n{proc.stdout}")
        if proc.stderr.strip():
            logger.info(f"--- stderr ---\n{proc.stderr}")
        if proc.returncode != 0:
            raise AssertionError(
                f"consensus live probe FAILED (exit {proc.returncode}):\n"
                f"{proc.stdout}\n{proc.stderr}")
        logger.info("consensus live probe PASSED.")

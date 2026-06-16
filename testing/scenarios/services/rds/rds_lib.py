"""Robot library for the RDS zero-copy data-plane roundtrip.

Runs the prebuilt rds_roundtrip binary (producer→shared-memory→consumer over the
iceoryx broker) and reports its verdict into Robot output. The C++ test owns WHAT
is asserted — a frame Loaned/filled/Published by the writer is Taken by the
reader at the SAME shared-memory address (zero copy), payload intact. Needs
iox-roudi running (the supervised broker); the test self-skips if it's down.

Build first: bazel build //services/rds/test:rds_roundtrip
"""
from __future__ import annotations

import subprocess
from pathlib import Path

from robot.api import logger
from robot.api.deco import keyword, library

_WS = Path(__file__).resolve().parents[4]


@library(scope="SUITE")
class RdsLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    @keyword("Require RouDi Running")
    def require_roudi_running(self) -> None:
        """Skip unless iox-roudi (the iceoryx broker) is up."""
        rc = subprocess.run(["pgrep", "-x", "iox-roudi"],
                            capture_output=True).returncode
        if rc != 0:
            from robot.api import SkipExecution
            raise SkipExecution("iox-roudi not running — start the stack "
                                "(`theia start`) or run iox-roudi.")

    @keyword("Run Rds Roundtrip")
    def run_rds_roundtrip(self) -> None:
        """Run the zero-copy roundtrip; FAIL on non-zero, surface output."""
        binary = _WS / "bazel-bin" / "services" / "rds" / "test" / "rds_roundtrip"
        if not binary.exists():
            raise AssertionError(
                f"not built: {binary}\nRun: bazel build //services/rds/test:rds_roundtrip")
        proc = subprocess.run([str(binary)], cwd=str(_WS),
                              capture_output=True, text=True, timeout=30)
        logger.info(f"--- rds_roundtrip ---\n{proc.stdout}\n{proc.stderr}")
        if proc.returncode != 0:
            raise AssertionError(
                f"rds roundtrip FAILED (exit {proc.returncode}):\n{proc.stdout}")
        if "rds-roundtrip: OK" not in proc.stdout:
            raise AssertionError(
                f"rds roundtrip did not confirm zero-copy:\n{proc.stdout}")
        logger.info("rds roundtrip: PASSED (zero-copy shared-mem confirmed).")

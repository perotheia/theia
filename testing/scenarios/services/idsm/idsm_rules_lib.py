"""Robot library for the IDSM rule-catalog implementation tests.

Runs the bazel-built security-plane unit tests (the prebuilt binaries, no bazel
re-invocation in the test lane — same pattern as the other idsm/com suites) and
surfaces pass/fail into Robot output. The C++ tests own WHAT is asserted; this
library gives uniform reporting so a rule-catalog regression reports alongside
every other scenario.

Covers the userspace (no-eBPF) v1: ProcDetector Cat A/C/D/H parse+classify+scope
+ the Cat-B nft-counter parse, and fw's per-FC egress-chain generation.

Build first: bazel build //services/idsm/test:test_proc_detector
                          //services/fw/test:test_egress_gen
"""
from __future__ import annotations

import subprocess
from pathlib import Path

from robot.api import logger
from robot.api.deco import keyword, library

# .../testing/scenarios/services/idsm/idsm_rules_lib.py → repo root (parents[4]).
_WS = Path(__file__).resolve().parents[4]


@library(scope="SUITE")
class IdsmRulesLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def _run_bin(self, rel: str, marker: str) -> None:
        binary = _WS / "bazel-bin" / rel
        if not binary.exists():
            raise AssertionError(
                f"test binary not built: {binary}\nRun: bazel build //{rel.replace('/', ':', 0)}")
        proc = subprocess.run([str(binary)], cwd=str(_WS),
                              capture_output=True, text=True, timeout=30)
        logger.info(f"--- {rel} stdout ---\n{proc.stdout}")
        if proc.stderr.strip():
            logger.info(f"--- stderr ---\n{proc.stderr}")
        if proc.returncode != 0:
            raise AssertionError(
                f"{rel} FAILED (exit {proc.returncode}):\n{proc.stdout}\n{proc.stderr}")
        if marker not in proc.stdout:
            raise AssertionError(
                f"{rel} did not reach its assertion (no '{marker}'):\n{proc.stdout}")

    @keyword("Run ProcDetector Test")
    def run_proc_detector_test(self) -> None:
        """ProcDetector Cat A/C/D/H parse/classify/scope + Cat-B counter parse."""
        self._run_bin("services/idsm/test/test_proc_detector", "proc-detector test: OK")

    @keyword("Run Fw Egress Gen Test")
    def run_fw_egress_gen_test(self) -> None:
        """fw per-FC egress output-chain generation (socket cgroupv2 + counter)."""
        self._run_bin("services/fw/test/test_egress_gen", "fw-egress-gen: OK")

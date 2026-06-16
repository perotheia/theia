"""Robot library for the com/crypto certificate-agreement check.

Wraps the bazel cc_test //services/com/test:test_cert_agreement so the agreement
reports into Robot output alongside every other scenario. The C++ test owns WHAT
is asserted (calls com_tls.hpp's crypto_get_cert against the live crypto FC and
asserts an undeployed slot → the NO_CERT agreement, never a silent CERT_OK or a
crypto-outage downgrade); this library RUNS it and maps pass/fail into report.html.

The agreement: com enables no-certificate (insecure) gRPC mode ONLY when crypto
explicitly replies 'no certificate deployed' (UNKNOWN_IDENTIFIER / empty PEM). A
crypto outage or a non-absence error fails CLOSED — com never opens an insecure
port on a guess.

Needs crypto listening (`theia start`); the test self-skips when crypto is
unreachable, so this is safe in a hermetic lane too.
"""
from __future__ import annotations

import subprocess
from pathlib import Path

from robot.api import logger
from robot.api.deco import keyword, library

# .../testing/scenarios/services/com/com_cert_agreement_lib.py → repo root:
# com[0] services[1] scenarios[2] testing[3] theia[4].
_WS = Path(__file__).resolve().parents[4]
_CRYPTO_TIPC_DEC = 0x80010006   # services/crypto CryptoProvider


@library(scope="SUITE")
class ComCertAgreementLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    @keyword("Require Crypto Listening")
    def require_crypto_listening(self) -> None:
        """Skip unless crypto is TIPC-bound (the stack is up)."""
        try:
            out = subprocess.run(
                ["tipc", "nametable", "show"],
                capture_output=True, text=True, timeout=10).stdout
        except (FileNotFoundError, subprocess.SubprocessError):
            logger.info("`tipc` unavailable — deferring to the test's own gate.")
            return
        if str(_CRYPTO_TIPC_DEC) not in out:
            from robot.api import SkipExecution
            raise SkipExecution(
                f"crypto not bound (TIPC {hex(_CRYPTO_TIPC_DEC)} absent) — "
                "start the stack with `theia start`.")

    @keyword("Run Cert Agreement Test")
    def run_cert_agreement_test(self) -> None:
        """Run the prebuilt cc_test binary; FAIL on non-zero, surface output.

        Runs the bazel-built binary directly (no bazel re-invocation in the test
        lane — same as how the idsm suite runs its python smoke). Build it first
        with `bazel build //services/com/test:test_cert_agreement`."""
        binary = _WS / "bazel-bin" / "services" / "com" / "test" / "test_cert_agreement"
        if not binary.exists():
            raise AssertionError(
                f"test binary not built: {binary}\n"
                "Run: bazel build //services/com/test:test_cert_agreement")
        proc = subprocess.run(
            [str(binary)], cwd=str(_WS), capture_output=True, text=True, timeout=30)
        logger.info(f"--- test_cert_agreement stdout ---\n{proc.stdout}")
        if proc.stderr.strip():
            logger.info(f"--- stderr ---\n{proc.stderr}")
        if proc.returncode != 0:
            raise AssertionError(
                f"cert-agreement test FAILED (exit {proc.returncode}):\n"
                f"{proc.stdout}\n{proc.stderr}")
        # Guard against a silent skip masquerading as success.
        if "cert-agreement: OK" not in proc.stdout:
            raise AssertionError(
                "cert-agreement test did not reach the live-crypto assertion "
                f"(no 'OK' marker — crypto may be down):\n{proc.stdout}")
        logger.info("cert-agreement: PASSED (NO_CERT agreement confirmed).")

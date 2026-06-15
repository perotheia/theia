"""Hermetic Robot library for the autosar regen-consistency selftest.

Drives `artheia gen-autosar-system` end-to-end on synthetic catalogs
(no vendor DBC/FIBEX dependency). Verifies:

- The `--package` flag flows into the emitted `package <name>` line.
- Re-running with identical inputs produces byte-identical output
  (no timestamps, no nondeterministic dict iteration order, etc).

Only used by autosar_regen_selftest.robot — not a user-facing keyword
surface.
"""
from __future__ import annotations

import json
import subprocess
from pathlib import Path

from robot.api.deco import keyword, library


@library(scope="SUITE")
class AutosarRegenLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self) -> None:
        self._tmpdir: Path | None = None

    @keyword("Set Up Synthetic Catalogs In")
    def setup_synthetic_catalogs(self, tmpdir: str) -> None:
        """Write two tiny catalog.json files (one CAN, one FlexRay)
        with three messages each. Mirrors the shape that
        artheia/artheia/generators/autosar_system.py expects:

            { "bus": <str>, "bus_kind": <str>, "messages": { <pdu>: ... } }
        """
        self._tmpdir = Path(tmpdir)
        self._tmpdir.mkdir(parents=True, exist_ok=True)
        kcan_catalog = {
            "bus": "synth_kcan",
            "bus_kind": "can",
            "messages": {
                "FOO_01": {"can_id": 100},
                "BAR_02": {"can_id": 101},
                "BAZ_03": {"can_id": 102},
            },
        }
        fibex_catalog = {
            "bus": "synth_fibex",
            "bus_kind": "flexray",
            "messages": {
                "ALPHA_01": {"slot_id": 10, "channel_idx": 0, "cycle": 1},
                "BETA_02":  {"slot_id": 11, "channel_idx": 0, "cycle": 1},
                "GAMMA_03": {"slot_id": 12, "channel_idx": 1, "cycle": 2},
            },
        }
        (self._tmpdir / "kcan_catalog.json").write_text(
            json.dumps(kcan_catalog, indent=2))
        (self._tmpdir / "fibex_catalog.json").write_text(
            json.dumps(fibex_catalog, indent=2))

    @keyword("Run Gen Autosar System")
    def run_gen_autosar_system(self, out_basename: str, package: str) -> str:
        """Invoke `artheia gen-autosar-system` against the synthetic
        catalogs. Returns the path of the emitted .art file."""
        assert self._tmpdir is not None, "call `Set Up Synthetic Catalogs In` first"
        out = self._tmpdir / out_basename
        cmd = [
            "artheia", "gen-autosar-system",
            "--catalog", str(self._tmpdir / "kcan_catalog.json"),
            "--catalog", str(self._tmpdir / "fibex_catalog.json"),
            "--out", str(out),
            "--package", package,
        ]
        result = subprocess.run(
            cmd, capture_output=True, text=True, check=False,
        )
        if result.returncode != 0:
            raise AssertionError(
                f"gen-autosar-system failed (rc={result.returncode}):\n"
                f"stdout: {result.stdout}\nstderr: {result.stderr}"
            )
        if not out.exists():
            raise AssertionError(f"expected output not written: {out}")
        return str(out)

    @keyword("Read Package Decl From Art")
    def read_package_decl(self, path: str) -> str:
        """Return the first `package <name>` line of the .art file.

        Catches drift: if the generator forgets to emit the --package
        value verbatim, the test fails.
        """
        for line in Path(path).read_text().splitlines():
            if line.startswith("package "):
                return line[len("package "):].strip()
        raise AssertionError(f"no `package ...` line in {path}")

    @keyword("Files Are Byte Identical")
    def files_are_byte_identical(self, a: str, b: str) -> None:
        """Idempotency assertion: re-running the generator on the same
        inputs must produce identical bytes. Catches regressions where
        someone introduces a timestamp or non-deterministic ordering."""
        ba = Path(a).read_bytes()
        bb = Path(b).read_bytes()
        if ba != bb:
            raise AssertionError(
                f"{a} and {b} differ ({len(ba)} vs {len(bb)} bytes). "
                f"Generator is non-deterministic — likely a dict-iteration "
                f"order, timestamp, or temp-path leak."
            )

    @keyword("Art File Forward Declares Pdu")
    def asserts_pdu_present(self, path: str, pdu_name: str) -> None:
        """Sanity-check that a known PDU made it through. We're not
        re-testing the generator's logic here — just that the wiring
        from catalog.json `messages` to .art `senderReceiver` works."""
        text = Path(path).read_text()
        iface_line = f"interface senderReceiver {pdu_name}_Iface"
        if iface_line not in text:
            raise AssertionError(
                f"{pdu_name} interface decl not in {path} — "
                f"catalog→.art wiring broke"
            )

"""Robot library for the FC regen-stability selftest.

The contract: every committed FC under services/<short>/{lib,main,impl}
MUST match what `artheia gen-app --kind fc` emits when run against the
sibling spec at services/<short>/system/package.art. Hand-edits to
generated files (lib/, main/, and the BUILD.bazel in impl/) are
forbidden — they cause silent drift and break the gen → build
dependency that the rest of the toolchain assumes.

This test regenerates each FC into /tmp, diff'ing every lib + main
file (including BUILD.bazel) byte-for-byte against the in-tree
committed version. The impl/<Daemon>_handlers.cc file is excluded
from the diff (it's user-owned business logic), but impl/BUILD.bazel
IS checked — it carries no business logic.

Drives all 5 FCs that exist today: sm, com, per, ucm, log.
"""
from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

from robot.api.deco import keyword, library


# FCs that should regenerate byte-identically. Each entry is
# (short, spec_path, namespace). spec_path is the .art that
# `artheia gen-app` should be invoked against.
FC_SPECS = [
    ("sm",  "services/system/sm/package.art",  "ara::sm"),
    ("com", "services/system/com/package.art", "ara::com"),
    ("per", "services/system/per/package.art", "ara::per"),
    ("ucm", "services/system/ucm/package.art", "ara::ucm"),
    ("log", "services/log/system/package.art", "ara::log"),
]


@library(scope="SUITE")
class FcRegenLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self) -> None:
        self._workspace: Path | None = None

    @keyword("Use Workspace")
    def use_workspace(self, path: str) -> None:
        self._workspace = Path(path).resolve()
        if not (self._workspace / "MODULE.bazel").exists():
            raise AssertionError(
                f"{self._workspace} doesn't look like a pero_theia checkout"
            )

    @keyword("Regen And Diff FC")
    def regen_and_diff_fc(self, short: str) -> None:
        """Regen one FC into /tmp and diff its lib/, main/, and
        impl/BUILD.bazel against the in-tree committed version.
        impl/<Daemon>_handlers.cc is excluded (user-owned)."""
        assert self._workspace is not None
        spec_path = next(
            (s, n) for k, s, n in FC_SPECS if k == short
        )
        spec, ns = spec_path

        scratch = Path("/tmp") / f"fc_regen_{short}"
        if scratch.exists():
            shutil.rmtree(scratch)
        scratch.mkdir(parents=True)

        env = os.environ.copy()
        env["PATH"] = f"{self._workspace}/.venv/bin:{env.get('PATH', '')}"
        env.pop("PYTHONPATH", None)
        result = subprocess.run(
            [
                "artheia", "gen-app", "--kind", "fc",
                str(self._workspace / spec),
                "--out", str(scratch),
                "--proto-out", str(scratch / "proto"),
                "--ns", ns,
            ],
            cwd=str(self._workspace), env=env,
            capture_output=True, text=True, check=False,
        )
        if result.returncode != 0:
            raise AssertionError(
                f"gen-app for {short} failed:\n"
                f"stdout: {result.stdout}\nstderr: {result.stderr}"
            )

        in_tree = self._workspace / "services" / short
        # Diff every regenerable file. Note we skip the source-path
        # comment because that's a cosmetic /tmp vs services/ path
        # difference, not real drift.
        regenerable = [
            ("lib", None),                            # all files
            ("main", None),                           # all files
            ("impl", "BUILD.bazel"),                  # only BUILD
        ]
        for slice_, filter_ in regenerable:
            tmp_slice = scratch / slice_
            in_tree_slice = in_tree / slice_
            for tmp_f in tmp_slice.iterdir():
                if filter_ and tmp_f.name != filter_:
                    continue
                in_tree_f = in_tree_slice / tmp_f.name
                if not in_tree_f.exists():
                    raise AssertionError(
                        f"{short}: gen-app emits {slice_}/{tmp_f.name}, "
                        f"but it doesn't exist in tree"
                    )
                tmp_text = self._strip_source_comment(tmp_f.read_text())
                in_tree_text = self._strip_source_comment(in_tree_f.read_text())
                if tmp_text != in_tree_text:
                    raise AssertionError(
                        f"{short}: {slice_}/{tmp_f.name} drift — "
                        f"the committed file diverges from gen-app output. "
                        f"Run `artheia gen-app --kind fc {spec} "
                        f"--out services/{short}/ --proto-out platform/proto/ "
                        f"--ns {ns} --force` and commit the diff."
                    )

    @staticmethod
    def _strip_source_comment(text: str) -> str:
        """gen-app's emit includes a `source: <path>` comment near
        the top of every file (sometimes embedded in a longer banner
        like `# FIRST-TIME-ONLY (regenerated with --force). source:
        ...`). We normalize the path portion so /tmp paths vs
        services/<fc>/ paths don't trip the diff."""
        import re
        # Replace everything from `source: ` to end-of-line with a
        # stable placeholder.
        return re.sub(
            r"source: [^\n]*",
            "source: <stripped>",
            text,
        )

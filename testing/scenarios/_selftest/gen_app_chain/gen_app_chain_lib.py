"""Hermetic Robot library for the gen-app application-generation
selftest.

Exercises the full lib / main / impl generation cycle for an
artheia-driven FC. Output lands under services/duo/ (in-tree, so
Bazel can find it); the test wipes the path before and after each
run so we never commit the synthetic FC.

Pipeline stages exercised:

  1. Initial gen-app emit (lib + main + impl + proto)
  2. Idempotent re-emit (lib + main overwritten, impl preserved)
  3. User edits impl → re-emit → impl bytes unchanged
  4. Re-emit with --force → impl overwritten
  5. Add a new signal to .art → re-emit → proto + lib reflect it,
     impl still compiles (handler signatures stable)
  6. Bazel build the produced cc_binary (live-bazel-tagged)

The "Force regenerate impl. Still compile." invariant from the
task spec is the strongest property check: the lib/main templates
are designed so that ANY .art change that doesn't break the public
node-class API leaves the user's impl handlers compilable.

Only used by gen_app_chain_selftest.robot.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

from robot.api.deco import keyword, library


@library(scope="SUITE")
class GenAppChainLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    # In-tree FC location. The Robot fixture wipes these dirs before
    # and after the suite so a failed run doesn't leave artifacts in
    # the worktree.
    FC_SHORT = "duo"
    FC_PACKAGE = "system.services.duo"
    FC_NAMESPACE = "ara::duo"

    def __init__(self) -> None:
        self._workspace: Path | None = None
        self._fc_dir: Path | None = None
        self._proto_dir: Path | None = None
        self._art_src: Path | None = None  # source .art (in the test dir)
        self._art_dst: Path | None = None  # destination under services/<fc>/system/

    # ------------------------------------------------------------------
    # Workspace anchoring + scratch path management
    # ------------------------------------------------------------------

    @keyword("Use Workspace")
    def use_workspace(self, path: str) -> None:
        self._workspace = Path(path).resolve()
        if not (self._workspace / "MODULE.bazel").exists():
            raise AssertionError(
                f"{self._workspace} doesn't look like a pero_theia checkout"
            )
        self._fc_dir = self._workspace / "services" / self.FC_SHORT
        self._proto_dir = self._workspace / "platform" / "proto"
        # The source .art ships next to this lib.
        self._art_src = (
            Path(__file__).parent / "duo_package.art"
        ).resolve()
        self._art_dst = self._fc_dir / "system" / "package.art"

    @keyword("Wipe Synthetic FC")
    def wipe_synthetic_fc(self) -> None:
        """Remove every file gen-app could have emitted for the duo FC.

        Idempotent — first call may have nothing to remove. Called
        in both Suite Setup AND Suite Teardown so a failed assertion
        doesn't leave artifacts in the worktree."""
        assert self._fc_dir is not None
        assert self._proto_dir is not None
        if self._fc_dir.exists():
            shutil.rmtree(self._fc_dir)
        # Also remove the proto landing under platform/proto/system/services/duo/
        proto_pkg_dir = (
            self._proto_dir / "system" / "services" / "duo"
        )
        if proto_pkg_dir.exists():
            shutil.rmtree(proto_pkg_dir)

    @keyword("Seed FC Source")
    def seed_fc_source(self) -> None:
        """Copy the test's duo_package.art into services/duo/system/."""
        assert self._art_src is not None and self._art_dst is not None
        self._art_dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(self._art_src, self._art_dst)

    # ------------------------------------------------------------------
    # gen-app driver
    # ------------------------------------------------------------------

    def _run_gen_app(self, *extra_args: str) -> str:
        assert self._workspace is not None
        env = os.environ.copy()
        env["PATH"] = f"{self._workspace}/.venv/bin:{env.get('PATH', '')}"
        env.pop("PYTHONPATH", None)
        result = subprocess.run(
            [
                "artheia", "gen-app", "--kind", "fc",
                str(self._art_dst),
                "--out", str(self._fc_dir),
                "--proto-out", str(self._proto_dir),
                "--ns", self.FC_NAMESPACE,
                *extra_args,
            ],
            cwd=str(self._workspace),
            env=env,
            capture_output=True, text=True, check=False,
        )
        if result.returncode != 0:
            raise AssertionError(
                f"gen-app failed (rc={result.returncode}):\n"
                f"stdout: {result.stdout}\nstderr: {result.stderr}"
            )
        return result.stdout

    @keyword("Run Gen App")
    def run_gen_app(self) -> str:
        """First-emit run — no --force."""
        return self._run_gen_app()

    @keyword("Run Gen App With Force")
    def run_gen_app_with_force(self) -> str:
        """--force overwrites impl/ + executor.py (write-once slices)."""
        return self._run_gen_app("--force")

    # ------------------------------------------------------------------
    # File-presence + content assertions
    # ------------------------------------------------------------------

    @keyword("Slice File Exists")
    def slice_file_exists(self, slice_: str, name: str) -> str:
        """services/duo/<slice>/<name> exists. Returns the path."""
        p = self._fc_dir / slice_ / name
        if not p.exists():
            raise AssertionError(f"missing: {p}")
        return str(p)

    @keyword("File Hash")
    def file_hash(self, path: str) -> str:
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()

    @keyword("File Has Autogen Marker")
    def file_has_autogen_marker(self, path: str) -> None:
        """All regenerable lib/ + main/ files carry an `AUTO-GENERATED`
        header. Impl files do NOT (they're user-owned). This check
        is a cheap regression guard against the templates losing
        their identification banner."""
        text = Path(path).read_text()
        if "AUTO-GENERATED" not in text.splitlines()[0]:
            raise AssertionError(
                f"{path} missing AUTO-GENERATED header on line 1; "
                f"head: {text.splitlines()[0]!r}"
            )

    @keyword("File Lacks Autogen Marker")
    def file_lacks_autogen_marker(self, path: str) -> None:
        """Impl files should NOT carry the AUTO-GENERATED banner —
        they're hand-edited."""
        text = Path(path).read_text()
        first = text.splitlines()[0] if text.splitlines() else ""
        if "AUTO-GENERATED" in first:
            raise AssertionError(
                f"{path} unexpectedly carries AUTO-GENERATED header: {first!r}"
            )

    @keyword("File Contains")
    def file_contains(self, path: str, needle: str) -> None:
        text = Path(path).read_text()
        if needle not in text:
            raise AssertionError(
                f"{path} does not contain {needle!r}"
            )

    @keyword("File Does Not Contain")
    def file_does_not_contain(self, path: str, needle: str) -> None:
        text = Path(path).read_text()
        if needle in text:
            raise AssertionError(
                f"{path} unexpectedly contains {needle!r}"
            )

    # ------------------------------------------------------------------
    # User-edit simulation
    # ------------------------------------------------------------------

    @keyword("Append User Comment To Handler")
    def append_user_comment(self, path: str, marker: str) -> None:
        """Simulate a user hand-edit by appending a unique marker
        comment. Used to test impl-preservation across regen."""
        with open(path, "a") as f:
            f.write(f"\n// USER-EDIT MARKER: {marker}\n")

    # ------------------------------------------------------------------
    # .art mutation
    # ------------------------------------------------------------------

    @keyword("Add Signal To Art")
    def add_signal_to_art(self, signal_name: str, message_name: str) -> None:
        """Append a new senderReceiver + a sender port on PongerNode
        (the daemon). PongerNode is what gen-app's lib template wires
        broadcasters/subscribers for, so the new interface MUST show up
        in lib/PingerNode.hh after re-emit.

        Wired on PongerNode (not PingerNode) deliberately — gen-app
        emits lib/main code for the statem-bearing node, so signals on
        the non-daemon peer land only in the proto. Adding the signal
        on the daemon catches lib-side regen drift too."""
        assert self._art_dst is not None
        text = self._art_dst.read_text()
        injection = (
            f"\nmessage {message_name} {{ uint32 v }}\n"
            f"interface senderReceiver {signal_name} "
            f"{{ data {message_name} msg }}\n"
        )
        text = text.replace(
            "interface clientServer DuoCtl {",
            injection + "\ninterface clientServer DuoCtl {",
        )
        # Add a new sender port to PongerNode (the daemon) so the new
        # interface drives the lib's broadcast/subscriber wiring.
        text = text.replace(
            "        sender   pong_out provides Pongs\n",
            "        sender   pong_out provides Pongs\n"
            f"        sender   {signal_name.lower()}_out provides {signal_name}\n",
        )
        self._art_dst.write_text(text)

    # ------------------------------------------------------------------
    # Bazel build
    # ------------------------------------------------------------------

    def _add_proto_to_umbrella(self) -> str:
        """Inject the duo proto target into platform/proto/BUILD.bazel's
        platform_protos srcs/hdrs lists. Returns the original content
        so the teardown can restore it."""
        assert self._workspace is not None
        bf = self._workspace / "platform" / "proto" / "BUILD.bazel"
        original = bf.read_text()
        if "duo:duo_pb_c" in original:
            return original  # already wired (idempotent)
        patched = original.replace(
            '"//platform/proto/system/services/exec:exec_pb_c",',
            '"//platform/proto/system/services/exec:exec_pb_c",\n'
            '        "//platform/proto/system/services/sm:duo_pb_c"'
            '.replace("sm", "duo"),  # injected by gen_app_chain test',
        )
        # Simpler: use a marker comment + bare label injection.
        patched = original.replace(
            '"//platform/proto/system/services/com:com_pb_c",',
            '"//platform/proto/system/services/com:com_pb_c",\n'
            '        "//platform/proto/system/services/duo:duo_pb_c",',
        ).replace(
            '"//platform/proto/system/services/com:com_pb_h",',
            '"//platform/proto/system/services/com:com_pb_h",\n'
            '        "//platform/proto/system/services/duo:duo_pb_h",',
        )
        bf.write_text(patched)
        return original

    def _emit_proto_build(self) -> None:
        """gen-app emits the .proto under platform/proto/system/services/duo/
        but NOT the BUILD.bazel sibling that nanopb_generator wraps. Hand-craft
        a BUILD.bazel that mirrors the per-FC proto BUILD pattern used by every
        existing FC (see platform/proto/system/services/sm/BUILD.bazel)."""
        assert self._proto_dir is not None
        target_dir = self._proto_dir / "system" / "services" / "duo"
        src_proto = target_dir / "duo.proto"
        if not src_proto.exists():
            raise AssertionError(
                f"gen-app didn't write expected proto at {src_proto}"
            )
        (target_dir / "BUILD.bazel").write_text("""\
# AUTO-GENERATED by gen_app_chain Robot test. Mirrors the per-FC proto
# BUILD pattern (see platform/proto/system/services/sm/BUILD.bazel).
package(default_visibility = ["//visibility:public"])

genrule(
    name = "duo_pb",
    srcs = ["duo.proto"],
    outs = ["duo.pb.c", "duo.pb.h"],
    cmd = "set -e;"
        + " in_dir=$$(dirname $(location duo.proto));"
        + " out_dir=$$(dirname $(location duo.pb.c));"
        + " nanopb_generator -I $$in_dir -D $$out_dir duo.proto;",
    local = True,
)

filegroup(name = "duo_pb_c", srcs = ["duo.pb.c"])
filegroup(name = "duo_pb_h", srcs = ["duo.pb.h"])
""")

    @keyword("Wire FC Into Bazel")
    def wire_fc_into_bazel(self) -> None:
        """Run after gen-app: hand-make the proto BUILD.bazel and
        register it in platform_protos so a cc_binary build will link
        cleanly. Stored so Unwire FC can restore.

        Per #382, gen-app's BUILD templates now target
        `//services/<short>/lib` directly (the canonical layout), so
        no symlink hack needed."""
        self._emit_proto_build()
        self._platform_protos_original = self._add_proto_to_umbrella()

    @keyword("Unwire FC From Bazel")
    def unwire_fc_from_bazel(self) -> None:
        """Tear down the in-tree mutations from Wire FC Into Bazel."""
        assert self._workspace is not None
        bf = self._workspace / "platform" / "proto" / "BUILD.bazel"
        orig = getattr(self, "_platform_protos_original", None)
        if orig is not None:
            bf.write_text(orig)

    @keyword("Bazel Build FC")
    def bazel_build_fc(self) -> None:
        """`bazel build //services/duo/main:duo`. Asserts the
        cc_binary actually links."""
        assert self._workspace is not None
        env = os.environ.copy()
        env["PATH"] = f"{self._workspace}/.venv/bin:{env.get('PATH', '')}"
        target = f"//services/{self.FC_SHORT}/main:{self.FC_SHORT}"
        result = subprocess.run(
            ["bazel", "build", target],
            cwd=str(self._workspace), env=env,
            capture_output=True, text=True, check=False,
        )
        if result.returncode != 0:
            raise AssertionError(
                f"bazel build {target} failed (rc={result.returncode}):\n"
                f"stdout: {result.stdout[-2000:]}\n"
                f"stderr: {result.stderr[-2000:]}"
            )

"""Robot library for the FC regen-stability selftest.

The contract: every committed FC's {lib,main,impl} MUST match what
`artheia gen-app --kind fc` emits when run against its `.art` spec.
Hand-edits to generated files (lib/, main/, and the BUILD.bazel in
impl/) are forbidden — they cause silent drift and break the gen →
build dependency that the rest of the toolchain assumes.

gen-app is path-agnostic: an FC can live ANYWHERE (services/, apps/,
platform/), and the generated cross-slice Bazel labels
are derived from --out. This harness mirrors that — each FC carries an
explicit out-path (and optional --composition), NOT a hardcoded
services/<short> shape.

This test regenerates each FC into /tmp, diff'ing every lib + main
file (including BUILD.bazel) byte-for-byte against the in-tree
committed version. The impl/<Daemon>_handlers.cc file is excluded
from the diff (it's user-owned business logic), but impl/BUILD.bazel
IS checked — it carries no business logic.

Drives the daemon FCs (sm, com, per, ucm, log) AND the non-services
FCs (the apps compositions, the gateway) — proving
gen-app stays byte-stable regardless of where the FC lives.
"""
from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

from robot.api.deco import keyword, library


@dataclass(frozen=True)
class FcSpec:
    """One FC the regen-stability contract covers.

    short:       test key (also the gen-app fc-short for services FCs).
    spec:        the .art passed to gen-app (workspace-relative).
    ns:          --ns C++ namespace.
    out:         --out tree, workspace-relative. The committed FC lives
                 here (or here/<composition> when composition is set —
                 gen-app appends it verbatim). NOT assumed to be
                 services/<short>; an FC lives anywhere.
    composition: --composition for a multi-composition spec (the demo);
                 None for a single-app FC.
    workspace:   which workspace the FC lives in, relative to the framework
                 checkout root. None ⇒ the framework root itself (services/
                 FCs). "demo" ⇒ the in-repo demo CONSUMING workspace, whose
                 app FCs (Demo3Way) moved out of the framework tree. The lib
                 runs gen-app with cwd / spec / in-tree resolved against this
                 workspace, and --proto-out set per-workspace.
    proto_out:   --proto-out tree, workspace-relative. The framework's
                 committed FCs were emitted with the proto tree under
                 platform/proto (that's where the //platform/proto:platform_protos
                 label the BUILD files reference resolves), so that's the
                 default. The demo workspace keeps proto at its root (proto/),
                 so the demo FCs override this to "proto".
    """
    short: str
    spec: str
    ns: str
    out: str
    composition: Optional[str] = None
    workspace: Optional[str] = None
    proto_out: str = "platform/proto"
    # impl/BUILD.bazel is normally regen-stable + checked. A few FCs HAND-OWN
    # it because they add real cc_library targets gen-app can't emit (e.g. per's
    # per_etcd / migration_registry isolation, log's shared trace_hub). For
    # those, impl/BUILD.bazel is user-owned business architecture — exclude it
    # from the regen diff (lib/ + main/ are still checked). The header on those
    # files already says "the user owns this BUILD.bazel."
    hand_owned_impl_build: bool = False

    @property
    def in_tree(self) -> str:
        """The committed FC directory, workspace-relative."""
        return f"{self.out}/{self.composition}" if self.composition else self.out


# FCs that should regenerate byte-identically. gen-app derives its
# cross-slice Bazel labels from --out, so the in-tree location is the
# single source of truth — services/ is no longer privileged.
FC_SPECS = [
    # Daemon FCs under services/. The log FC spec carries BOTH the
    # LogDaemon (syslog sink) and TraceCollector (trace fan-out) nodes.
    FcSpec("sm",  "system/services/sm/package.art",   "ara::sm",  "services/sm"),
    FcSpec("com", "system/services/com/package.art", "ara::com", "services/com"),
    FcSpec("crypto", "system/services/crypto/package.art", "ara::crypto",
           "services/crypto", hand_owned_impl_build=True),   # OpenSSL linkopts
    FcSpec("per", "system/services/per/package.art", "ara::per", "services/per",
           hand_owned_impl_build=True),   # per_etcd + migration_registry targets
    FcSpec("ucm", "system/services/ucm/package.art", "ara::ucm", "services/ucm"),
    FcSpec("log", "system/services/log/package.art", "ara::log", "services/log",
           hand_owned_impl_build=True),   # shared trace_hub cc_library
    FcSpec("tsync", "system/services/tsync/package.art", "ara::tsync",
           "services/tsync", hand_owned_impl_build=True),   # ptp_backend.hpp hdr
    FcSpec("nm", "system/services/nm/package.art", "ara::nm", "services/nm",
           hand_owned_impl_build=True),   # nm_backend.hpp hdr (the `ip` observer)
    FcSpec("shwa", "system/services/shwa/package.art", "ara::shwa",
           "services/shwa", hand_owned_impl_build=True),   # host/jetson backend select
    FcSpec("osi", "system/services/osi/package.art", "ara::osi", "services/osi",
           hand_owned_impl_build=True),   # osi_backend.hpp hdr (cgroup/power)
    FcSpec("fw", "system/services/fw/package.art", "ara::fw", "services/fw",
           hand_owned_impl_build=True),   # fw_backend.hpp hdr (nftables gen/apply)
    FcSpec("idsm", "system/services/idsm/package.art", "ara::idsm",
           "services/idsm", hand_owned_impl_build=True),   # ids_backend.hpp + log dep
    # log carries the SECURITY TraceKind IDSM produces; keep it regen-checked.
    # rds exercises the `node prebuilt` primitive: RoudiBroker (forks iox-roudi,
    # fully-generated fork/drain runnable, no state.hh) + RdsCtl (atomic). Proves
    # the prebuilt template-pair + impl/BUILD `not n.prebuilt` state-skip stay
    # regen-stable. The ara::rds transport lib lives in services/rds/transport
    # (a sibling target), not impl/BUILD, so impl/BUILD is gen-app-default.
    FcSpec("rds", "system/services/rds/package.art", "ara::rds", "services/rds"),
    # Non-services FCs — same generator, different home AND a different
    # workspace. The in-repo demo app moved out of the framework tree into a
    # CONSUMING workspace at demo/, so its .art (demo/system/apps/component.art)
    # and committed apps (demo/apps/Demo3WayP<N>) are resolved against the demo
    # workspace, not the framework root. The C++ namespace is `system_apps` —
    # the demo's gen-app default derived from the system.apps package path (the
    # committed apps were emitted with --ns system_apps). These still prove
    # gen-app's path-agnostic label derivation (//<out>/lib:<short>_lib): the
    # apps spec is one spec with three process-compositions, each its own app
    # dir via --composition (appended to --out verbatim).
    FcSpec("demo_p1", "system/apps/component.art", "system_apps", "apps",
           "Demo3WayP1", workspace="demo", proto_out="proto"),
    FcSpec("demo_p2", "system/apps/component.art", "system_apps", "apps",
           "Demo3WayP2", workspace="demo", proto_out="proto"),
    FcSpec("demo_p3", "system/apps/component.art", "system_apps", "apps",
           "Demo3WayP3", workspace="demo", proto_out="proto"),
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
        fc = next((f for f in FC_SPECS if f.short == short), None)
        if fc is None:
            raise AssertionError(f"unknown FC short {short!r}")

        scratch = Path("/tmp") / f"fc_regen_{short}"
        if scratch.exists():
            shutil.rmtree(scratch)
        scratch.mkdir(parents=True)

        # Which workspace this FC lives in. services/ FCs live in the
        # framework checkout (Use Workspace anchors there); the demo app FCs
        # moved out into the demo CONSUMING workspace at <framework>/demo. The
        # .art spec + committed FC dir are resolved against THIS workspace.
        fc_ws = self._workspace / fc.workspace if fc.workspace else self._workspace

        # The cross-slice Bazel labels gen-app emits are derived from the
        # --out STRING (//<out>/lib:<short>_lib), so to reproduce the
        # committed FC byte-for-byte we must pass the SAME workspace-
        # relative --out the real invocation uses (e.g. `apps`, not an
        # absolute /tmp path). We run with cwd=scratch and an ABSOLUTE
        # spec path, so the relative --out writes into scratch while the
        # baked-in labels stay `//<fc.out>/...`. --composition is appended
        # exactly as the real invocation does (gen-app appends it to --out
        # both for the filesystem and the label prefix).
        cmd = [
            "artheia", "gen-app", "--kind", "fc",
            str(fc_ws / fc.spec),
            "--out", fc.out,                       # workspace-relative — drives labels
            "--proto-out", fc.proto_out,
            "--ns", fc.ns,
        ]
        if fc.composition:
            cmd += ["--composition", fc.composition]

        env = os.environ.copy()
        # artheia is the FRAMEWORK console script — its venv lives at the
        # framework root regardless of which workspace the FC belongs to.
        env["PATH"] = f"{self._workspace}/.venv/bin:{env.get('PATH', '')}"
        env.pop("PYTHONPATH", None)
        result = subprocess.run(
            cmd, cwd=str(scratch), env=env,     # write into scratch, labels stay relative
            capture_output=True, text=True, check=False,
        )
        if result.returncode != 0:
            raise AssertionError(
                f"gen-app for {short} failed:\n"
                f"stdout: {result.stdout}\nstderr: {result.stderr}"
            )

        # gen-app wrote to scratch/<fc.in_tree>; the committed copy is at
        # <fc_ws>/<fc.in_tree>.
        regen_dir = scratch / fc.in_tree
        in_tree = fc_ws / fc.in_tree
        # Diff every regenerable file. Note we skip the source-path
        # comment because that's a cosmetic /tmp vs services/ path
        # difference, not real drift.
        regenerable = [
            ("lib", None),                            # all files
            ("main", None),                           # all files
        ]
        # impl/BUILD.bazel is checked unless this FC hand-owns it (real
        # cc_library targets gen-app can't reproduce — see FcSpec).
        if not fc.hand_owned_impl_build:
            regenerable.append(("impl", "BUILD.bazel"))
        for slice_, filter_ in regenerable:
            tmp_slice = regen_dir / slice_
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
                # A HAND-OWNED slice file is deliberately not the gen-app
                # template (e.g. sm's main.cc wires the two-node FSM via
                # LocalRef + sm_statem_ref()). Its lib/ + impl/BUILD still
                # regen byte-stable and ARE checked; only the hand-owned
                # file itself is exempt. The banner is the contract.
                if "HAND-OWNED" in in_tree_f.read_text(errors="ignore"):
                    continue
                tmp_text = self._strip_source_comment(tmp_f.read_text())
                in_tree_text = self._strip_source_comment(in_tree_f.read_text())
                if tmp_text != in_tree_text:
                    comp = f" --composition {fc.composition}" if fc.composition else ""
                    raise AssertionError(
                        f"{short}: {slice_}/{tmp_f.name} drift — "
                        f"the committed file diverges from gen-app output. "
                        f"Run `artheia gen-app --kind fc {fc.spec} "
                        f"--out {fc.out}/ --proto-out platform/proto/ "
                        f"--ns {fc.ns}{comp} --force` and commit the diff."
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

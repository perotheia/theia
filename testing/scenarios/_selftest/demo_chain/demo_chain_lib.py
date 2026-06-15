"""Hermetic Robot library for the demo .art→.ipk chain selftest.

Drives each stage of the Demo3Way generation pipeline against a
fresh /tmp workdir so the assertions don't depend on any committed
output state. Each keyword does ONE stage; the test cases compose
them so a failure points at the first broken hop.

Pipeline stages (mirrors the architecture map):

  1. artheia parse              — .art syntax + AST shape
  2. artheia rig-deps           — rig.json (Bazel + GUI feed)
  3. artheia gen-netgraph       — per-cluster signal routing JSON
  4. artheia gen-routing        — per-process LocalRef/RemoteRef hh
  5. artheia gen-app-composition — per-process CMakeLists + main.cc
  6. artheia generate-manifest  — dist/manifest/<m>/{4 yamls + index}
  7. artheia executor emit      — per-machine execution.yaml tree
  8. bazel build ...:image      — .ipk with arch tag matching rig.py

Only used by demo_chain_selftest.robot.
"""
from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path

from robot.api.deco import keyword, library


@library(scope="SUITE")
class DemoChainLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    # ------------------------------------------------------------------
    # Workspace + workdir
    # ------------------------------------------------------------------

    def __init__(self) -> None:
        self._workdir: Path | None = None
        self._workspace: Path | None = None

    @keyword("Use Workspace")
    def use_workspace(self, path: str) -> None:
        """Anchor at the pero_theia checkout root. All artheia paths
        below are workspace-relative, so we cd to here before each
        subprocess call."""
        self._workspace = Path(path).resolve()
        if not (self._workspace / "MODULE.bazel").exists():
            raise AssertionError(
                f"{self._workspace} doesn't look like a pero_theia checkout"
            )

    @keyword("Use Workdir")
    def use_workdir(self, path: str) -> None:
        """Fresh scratch dir for all generated artifacts. Created if
        missing; existing contents are left in place so a re-run can
        pick up the previous turn's outputs for diff-debugging."""
        self._workdir = Path(path)
        self._workdir.mkdir(parents=True, exist_ok=True)

    def _artheia(self, *args: str) -> subprocess.CompletedProcess:
        """Run `artheia ...` from the workspace root with the venv on PATH.

        Strips PYTHONPATH from the subprocess env so the rf-theia venv's
        sys.path (which Robot exposes via PYTHONPATH=.) doesn't shadow
        the artheia package's `__version__` import."""
        assert self._workspace is not None, "call `Use Workspace` first"
        env = os.environ.copy()
        env["PATH"] = f"{self._workspace}/.venv/bin:{env.get('PATH', '')}"
        env.pop("PYTHONPATH", None)
        return subprocess.run(
            ["artheia", *args],
            cwd=str(self._workspace),
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )

    def _ok(self, result: subprocess.CompletedProcess, what: str) -> None:
        if result.returncode != 0:
            raise AssertionError(
                f"{what} failed (rc={result.returncode}):\n"
                f"stdout: {result.stdout}\nstderr: {result.stderr}"
            )

    # ------------------------------------------------------------------
    # Stage 1 — parse component.art
    # ------------------------------------------------------------------

    @keyword("Stage 1 Parse Component Art")
    def stage1_parse(self) -> str:
        """Run `artheia parse apps/system/apps/component.art` and
        return the tree dump. Validates grammar + import resolution
        for the demo cluster + 3 compositions."""
        r = self._artheia("parse", "apps/system/apps/component.art")
        self._ok(r, "stage 1: artheia parse")
        return r.stdout

    @keyword("Tree Mentions")
    def tree_mentions(self, tree: str, token: str) -> None:
        if token not in tree:
            raise AssertionError(
                f"`artheia parse` output missing {token!r}; got:\n"
                f"{tree[:500]}..."
            )

    # ------------------------------------------------------------------
    # Stage 2 — rig-deps JSON
    # ------------------------------------------------------------------

    @keyword("Stage 2 Rig Deps")
    def stage2_rig_deps(self) -> str:
        """Emit rig.json — the Bazel rig_ext extension's input."""
        assert self._workdir is not None, "call `Use Workdir` first"
        out = self._workdir / "rig.json"
        r = self._artheia(
            "rig-deps", "apps.manifest.rig", "--out", str(out)
        )
        self._ok(r, "stage 2: artheia rig-deps")
        if not out.exists():
            raise AssertionError(f"rig.json not written at {out}")
        return str(out)

    @keyword("Json Has Machines")
    def rig_has_machines(self, path: str, *names: str) -> None:
        doc = json.loads(Path(path).read_text())
        got = {m["name"] for m in doc.get("machines", [])}
        missing = [n for n in names if n not in got]
        if missing:
            raise AssertionError(
                f"rig.json machines = {got}; missing {missing}"
            )

    @keyword("Json Has Component")
    def rig_has_component(self, path: str, component_name: str,
                          on_machine: str) -> None:
        doc = json.loads(Path(path).read_text())
        for c in doc.get("flat_components", []):
            if c["name"] == component_name and c["machine"] == on_machine:
                return
        raise AssertionError(
            f"rig.json flat_components has no {component_name!r} "
            f"on machine {on_machine!r}"
        )

    @keyword("Machine Arch Is")
    def machine_arch_is(self, path: str, machine_name: str,
                        expected_arch: str) -> None:
        doc = json.loads(Path(path).read_text())
        for m in doc["machines"]:
            if m["name"] == machine_name:
                got = m.get("arch")
                if got != expected_arch:
                    raise AssertionError(
                        f"machine {machine_name!r} arch = {got!r}, "
                        f"expected {expected_arch!r}"
                    )
                return
        raise AssertionError(f"machine {machine_name!r} not in rig.json")

    # ------------------------------------------------------------------
    # Stage 3 — gen-netgraph
    # ------------------------------------------------------------------

    @keyword("Stage 3 Gen Netgraph")
    def stage3_gen_netgraph(self) -> str:
        """Emit netgraph JSON describing nodes + cluster routing."""
        assert self._workdir is not None
        out = self._workdir / "demo_netgraph.json"
        r = self._artheia(
            "gen-netgraph", "apps/system/apps/component.art",
            "--out", str(out),
        )
        self._ok(r, "stage 3: artheia gen-netgraph")
        return str(out)

    @keyword("Netgraph Has Nodes")
    def netgraph_has_nodes(self, path: str, *names: str) -> None:
        doc = json.loads(Path(path).read_text())
        got = {n.get("name") for n in doc.get("nodes", [])}
        missing = [n for n in names if n not in got]
        if missing:
            raise AssertionError(
                f"netgraph nodes = {got}; missing {missing}"
            )

    # ------------------------------------------------------------------
    # Stage 4 — gen-routing
    # ------------------------------------------------------------------

    @keyword("Stage 4 Gen Routing")
    def stage4_gen_routing(self, composition: str) -> str:
        """Emit per-process LocalRef/RemoteRef headers for one
        composition (Demo3WayP1/P2/P3)."""
        assert self._workdir is not None
        out_dir = self._workdir / "routing"
        out_dir.mkdir(exist_ok=True)
        r = self._artheia(
            "gen-routing", "apps/system/apps/component.art",
            "--composition", composition,
            "--out", str(out_dir),
        )
        self._ok(r, f"stage 4: artheia gen-routing --composition {composition}")
        return str(out_dir)

    @keyword("Routing Header Exists")
    def routing_header_exists(self, out_dir: str, composition: str,
                              process: str) -> None:
        # gen-routing names files Demo3Way__<P>_refs.hh per the demo
        # convention (composition + process suffix).
        candidates = list(Path(out_dir).glob(f"*{process}*refs.hh"))
        if not candidates:
            raise AssertionError(
                f"no *_{process}*refs.hh in {out_dir}; "
                f"saw {[p.name for p in Path(out_dir).iterdir()]}"
            )

    # ------------------------------------------------------------------
    # Stage 5 — gen-app-composition
    # ------------------------------------------------------------------

    @keyword("Stage 5 Gen App Composition")
    def stage5_gen_app_composition(self, composition: str) -> str:
        """Emit per-process CMake project skeletons (main.cc +
        CMakeLists.txt). One project per `on process P` partition."""
        assert self._workdir is not None
        out_dir = self._workdir / f"app_{composition.lower()}"
        out_dir.mkdir(exist_ok=True)
        r = self._artheia(
            "gen-app-composition", "apps/system/apps/component.art",
            "--composition", composition,
            "--out", str(out_dir),
        )
        self._ok(r, f"stage 5: artheia gen-app-composition --composition {composition}")
        return str(out_dir)

    @keyword("App Composition Has Process Dir")
    def app_comp_has_process(self, out_dir: str, process: str) -> None:
        candidates = [p for p in Path(out_dir).iterdir() if p.is_dir()
                      and process.lower() in p.name.lower()]
        if not candidates:
            raise AssertionError(
                f"no process-dir matching {process!r} in {out_dir}; "
                f"saw {[p.name for p in Path(out_dir).iterdir()]}"
            )

    # ------------------------------------------------------------------
    # Stage 6 — generate-manifest (per-machine YAML set)
    # ------------------------------------------------------------------

    @keyword("Stage 6 Generate Manifest")
    def stage6_generate_manifest(self) -> str:
        """Emit the per-machine deploy manifest set:
        <out>/<machine>/{machine,application,service,execution}.yaml + index.yaml."""
        assert self._workdir is not None
        out = self._workdir / "manifest"
        r = self._artheia(
            "generate-manifest", "apps.manifest.rig",
            "--out", str(out),
        )
        self._ok(r, "stage 6: artheia generate-manifest")
        return str(out)

    @keyword("Manifest Has Machine Jsons")
    def manifest_has_machine_jsons(self, root: str, machine: str) -> None:
        """Every machine dir must have the 4 AUTOSAR-adaptive manifest
        JSON files. The supervisor and supervisor-gui both consume
        these — see platform/supervisor/src/spec.cpp and
        supervisor-gui/src/machines.cpp."""
        mdir = Path(root) / machine
        if not mdir.is_dir():
            raise AssertionError(f"no machine dir at {mdir}")
        expected = ["machine.json", "application.json",
                    "service.json", "execution.json"]
        missing = [f for f in expected if not (mdir / f).exists()]
        if missing:
            raise AssertionError(
                f"{mdir} missing {missing}; "
                f"saw {[p.name for p in mdir.iterdir()]}"
            )

    @keyword("No Yaml Emitted")
    def no_yaml_emitted(self, root: str) -> None:
        """JSON is the single canonical format after #380. Catches
        regressions where someone re-introduces YAML emit."""
        yamls = list(Path(root).rglob("*.yaml")) + list(Path(root).rglob("*.yml"))
        if yamls:
            raise AssertionError(
                f"YAML emit re-introduced — saw {[str(p) for p in yamls]}"
            )

    @keyword("Json Kind Is")
    def json_kind_is(self, root: str, machine: str, stem: str,
                     expected_kind: str) -> None:
        """Assert dist/manifest/<machine>/<stem>.json's top-level `kind`
        equals the expected manifest type tag."""
        import json as _json
        jp = Path(root) / machine / f"{stem}.json"
        doc = _json.loads(jp.read_text())
        kind = doc.get("kind")
        if kind != expected_kind:
            raise AssertionError(
                f"{jp} kind = {kind!r}, expected {expected_kind!r}"
            )

    @keyword("Index Json Lists Machines")
    def index_json_lists_machines(self, root: str, *names: str) -> None:
        """Top-level index.json mirrors index.yaml — Puppet's bootstrap
        uses it to find each machine's directory."""
        import json as _json
        jp = Path(root) / "index.json"
        if not jp.exists():
            raise AssertionError(f"missing top-level index.json: {jp}")
        doc = _json.loads(jp.read_text())
        if doc.get("kind") != "RigIndex":
            raise AssertionError(
                f"{jp} kind = {doc.get('kind')!r}, expected 'RigIndex'"
            )
        got = {m["name"] for m in doc.get("machines", [])}
        missing = [n for n in names if n not in got]
        if missing:
            raise AssertionError(
                f"index.json machines = {got}; missing {missing}"
            )

    @keyword("Execution Json Lists Process")
    def execution_lists_process(self, root: str, machine: str,
                                process: str) -> None:
        import json as _json
        path = Path(root) / machine / "execution.json"
        doc = _json.loads(path.read_text())
        names = {p.get("name") for p in doc.get("processes", [])}
        if process not in names:
            raise AssertionError(
                f"{path} doesn't list a process named {process!r}; "
                f"saw {sorted(names)}"
            )

    # ------------------------------------------------------------------
    # Stage 7 — executor emit (per-machine supervisor tree)
    # ------------------------------------------------------------------

    @keyword("Stage 7 Executor Emit")
    def stage7_executor_emit(self, machine: str) -> str:
        """Emit the supervisor tree YAML for ONE machine. This is the
        per-machine slice (#287) — pinned SupervisorNodes whose host
        doesn't match are dropped."""
        assert self._workdir is not None
        out = self._workdir / f"executor_{machine}.json"
        r = self._artheia(
            "executor", "emit", "apps.manifest.rig",
            "--machine", machine,
            "--out", str(out),
        )
        self._ok(r, f"stage 7: artheia executor emit --machine {machine}")
        return str(out)

    @keyword("Executor Json Root Strategy Is")
    def executor_root_strategy(self, path: str, expected: str) -> None:
        import json as _json
        doc = _json.loads(Path(path).read_text())
        got = doc.get("strategy")
        if got != expected:
            raise AssertionError(
                f"{path} root strategy = {got!r}, expected {expected!r}"
            )

    # ------------------------------------------------------------------
    # Stage 8 — bazel build :image (live, gated)
    # ------------------------------------------------------------------

    @keyword("Stage 8 Bazel Build Image")
    def stage8_bazel_build(self, machine: str) -> str:
        """`bazel build @rig_apps//<machine>:image` and return the
        produced .ipk path via `bazel cquery --output=files`."""
        assert self._workspace is not None
        env = os.environ.copy()
        env["PATH"] = f"{self._workspace}/.venv/bin:{env.get('PATH', '')}"
        target = f"@rig_apps//{machine}:image"
        b = subprocess.run(
            ["bazel", "build", target],
            cwd=str(self._workspace), env=env,
            capture_output=True, text=True, check=False,
        )
        if b.returncode != 0:
            raise AssertionError(
                f"stage 8: bazel build {target} failed (rc={b.returncode}):\n"
                f"stdout: {b.stdout}\nstderr: {b.stderr}"
            )
        q = subprocess.run(
            ["bazel", "cquery", target, "--output=files"],
            cwd=str(self._workspace), env=env,
            capture_output=True, text=True, check=False,
        )
        if q.returncode != 0:
            raise AssertionError(f"cquery failed: {q.stderr}")
        rel = q.stdout.strip().splitlines()[0] if q.stdout.strip() else ""
        if not rel:
            raise AssertionError(
                f"stage 8: cquery returned no path for {target}: {q.stdout!r}"
            )
        # cquery returns a workspace-relative bazel-out/... path; resolve
        # from the workspace root, not the lib's cwd.
        ipk = self._workspace / rel
        if not ipk.exists():
            raise AssertionError(
                f"stage 8: .ipk path from cquery doesn't exist: {ipk}"
            )
        return str(ipk)

    @keyword("Ipk Name Carries Arch")
    def ipk_carries_arch(self, ipk_path: str, expected_arch: str) -> None:
        """Sanity: arm64 rig-declared machine produces _arm64.ipk, etc."""
        name = Path(ipk_path).name
        token = f"_{expected_arch}.ipk"
        if not name.endswith(token):
            raise AssertionError(
                f"{name} doesn't end with {token!r} — rig.py arch "
                f"plumbing (#371) regressed?"
            )

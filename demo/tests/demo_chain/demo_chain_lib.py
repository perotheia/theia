"""Hermetic Robot library for the demo .art→.ipk chain selftest.

Drives each stage of the Demo3Way generation pipeline against a
fresh /tmp workdir so the assertions don't depend on any committed
output state. Each keyword does ONE stage; the test cases compose
them so a failure points at the first broken hop.

This is a DEMO-APP test and runs FROM the demo consuming workspace
(demo/). All artheia paths are workspace-relative to demo/:

  * .art source        system/apps/component.art
  * rig module         manifest.single.rig   (DeploymentLayer, attr RIG;
                                              manifest/ is a namespace pkg at
                                              the demo root — services half is
                                              symlinked in from the framework)

artheia is the FRAMEWORK's console script — there is no demo/.venv, so
we put the framework venv (<demo>/../.venv/bin, i.e. THEIA_ROOT/.venv)
on PATH. Subprocesses run with cwd=demo/ and PYTHONPATH=. so the rig
module imports.

Pipeline stages (mirrors the architecture map):

  1. artheia parse              — .art syntax + AST shape
  2. artheia rig-deps           — rig.json (Bazel + GUI feed)
  3. artheia gen-netgraph       — per-cluster signal routing JSON
  4. artheia gen-routing        — per-process LocalRef/RemoteRef hh
  5. artheia gen-app            — per-process app skeleton (lib/main/impl)
  6. artheia serialize-manifest — DeploymentLayer rig → dist/manifest/<m>/
                                  {machine,execution,service,application,
                                  executor}.json + a top-level machines.json.
                                  (The orthogonal-engine redesign folded the
                                  old `generate-manifest` + `executor emit`
                                  into this one validate-then-write command;
                                  the per-machine supervisor tree is now the
                                  <machine>/executor.json slice.)
  8. bazel build ...:image      — .ipk with arch tag matching rig.py

The rig is a dotted module path to a DeploymentLayer (manifest.single.rig,
attr RIG) — NOT an .art file. `manifest.services` (framework) + `manifest.apps`
(this workspace) resolve as one `manifest` namespace from the demo root.

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
        """Anchor at the demo consuming workspace root (the dir holding
        MODULE.bazel + system/apps/component.art + manifest/). All
        artheia paths below are workspace-relative, so we cd to here
        before each subprocess call."""
        self._workspace = Path(path).resolve()
        if not (self._workspace / "MODULE.bazel").exists():
            raise AssertionError(
                f"{self._workspace} doesn't look like a theia workspace"
            )

    @keyword("Use Workdir")
    def use_workdir(self, path: str) -> None:
        """Fresh scratch dir for all generated artifacts. Created if
        missing; existing contents are left in place so a re-run can
        pick up the previous turn's outputs for diff-debugging."""
        self._workdir = Path(path)
        self._workdir.mkdir(parents=True, exist_ok=True)

    def _venv_bin(self) -> Path:
        """The framework venv's bin dir. The demo workspace has no
        venv of its own; artheia is the framework console script living
        at THEIA_ROOT/.venv/bin (THEIA_ROOT == <demo>/..)."""
        assert self._workspace is not None
        return self._workspace.parent / ".venv" / "bin"

    def _artheia(self, *args: str) -> subprocess.CompletedProcess:
        """Run `artheia ...` from the demo workspace root with the
        framework venv on PATH.

        Sets PYTHONPATH=. so the demo's rig modules (manifest.single.rig
        etc.) import — `manifest/` is a namespace package at the workspace
        root spanning the demo's apps + the framework's services (the
        latter symlinked in as manifest/services). The artheia package
        itself comes from the venv, so prepending '.' doesn't shadow it."""
        assert self._workspace is not None, "call `Use Workspace` first"
        env = os.environ.copy()
        env["PATH"] = f"{self._venv_bin()}:{env.get('PATH', '')}"
        env["PYTHONPATH"] = "."
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
        """Run `artheia parse system/apps/component.art` and
        return the tree dump. Validates grammar + import resolution
        for the demo cluster + its process compositions."""
        r = self._artheia("parse", "system/apps/component.art")
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
            "rig-deps", "manifest.single.rig", "--out", str(out)
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
            "gen-netgraph", "system/apps/component.art",
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
            "gen-routing", "system/apps/component.art",
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
    # Stage 5 — gen-app (per-process app skeleton)
    # ------------------------------------------------------------------

    @keyword("Stage 5 Gen App Composition")
    def stage5_gen_app_composition(self, composition: str) -> str:
        """Emit the per-process app skeleton (lib/main/impl) for one
        composition via `gen-app --kind fc --composition`. gen-app
        appends the composition to --out, so the project lands at
        <out>/<composition>/. One project per `on process P` partition."""
        assert self._workdir is not None
        out_dir = self._workdir / "app"
        out_dir.mkdir(exist_ok=True)
        r = self._artheia(
            "gen-app", "--kind", "fc",
            "system/apps/component.art",
            "--out", str(out_dir),
            "--proto-out", str(self._workdir / "proto"),
            "--ns", "demo",
            "--composition", composition,
        )
        self._ok(r, f"stage 5: artheia gen-app --composition {composition}")
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
    # Stage 6 — serialize-manifest (per-machine JSON set)
    # ------------------------------------------------------------------

    @keyword("Stage 6 Serialize Manifest")
    def stage6_serialize_manifest(self, rig: str = "manifest.single.rig",
                                  attr: str = "RIG") -> str:
        """Run the orthogonal-engine serializer: a DeploymentLayer rig
        module → the per-machine deploy JSON set. Replaces the old
        `generate-manifest` + `executor emit` pair — `validate()` runs
        first (any error aborts non-zero), then it writes
        <out>/<machine>/{machine,execution,service,application,
        executor}.json + a top-level machines.json."""
        assert self._workdir is not None
        out = self._workdir / "manifest"
        r = self._artheia(
            "serialize-manifest", rig, "--attr", attr, "--out", str(out),
        )
        self._ok(r, f"stage 6: artheia serialize-manifest {rig} --attr {attr}")
        return str(out)

    @keyword("Manifest Has Machine Jsons")
    def manifest_has_machine_jsons(self, root: str, machine: str) -> None:
        """Every machine dir must have the 5 deploy JSON files. The
        supervisor + supervisor-gui consume machine/execution/service/
        application; executor.json is the per-machine supervisor tree
        slice (formerly the standalone `executor emit` output)."""
        mdir = Path(root) / machine
        if not mdir.is_dir():
            raise AssertionError(f"no machine dir at {mdir}")
        expected = ["machine.json", "application.json", "service.json",
                    "execution.json", "executor.json"]
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

    @keyword("Json Top Key Is")
    def json_top_key_is(self, root: str, machine: str, stem: str,
                        expected_key: str) -> None:
        """Assert dist/manifest/<machine>/<stem>.json carries its
        defining top-level key. The orthogonal-engine serializer emits
        the bare payload (no `kind` discriminator) — each file's identity
        IS its top-level container key: execution.json→processes,
        executor.json→supervisors, etc."""
        import json as _json
        jp = Path(root) / machine / f"{stem}.json"
        doc = _json.loads(jp.read_text())
        if expected_key not in doc:
            raise AssertionError(
                f"{jp} top keys = {sorted(doc)}; expected {expected_key!r}"
            )

    @keyword("Index Json Lists Machines")
    def index_json_lists_machines(self, root: str, *names: str) -> None:
        """Top-level machines.json is the machine-name list — the deploy
        bootstrap uses it to find each machine's directory. The
        orthogonal-engine serializer writes a bare {"machines": [name,...]}
        (no RigIndex `kind` wrapper)."""
        import json as _json
        jp = Path(root) / "machines.json"
        if not jp.exists():
            raise AssertionError(f"missing top-level machines.json: {jp}")
        doc = _json.loads(jp.read_text())
        got = set(doc.get("machines", []))
        missing = [n for n in names if n not in got]
        if missing:
            raise AssertionError(
                f"machines.json machines = {got}; missing {missing}"
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
    # Stage 7 — per-machine supervisor tree (executor.json slice)
    # ------------------------------------------------------------------

    @keyword("Executor Slice For Machine")
    def executor_slice_for_machine(self, root: str, machine: str) -> str:
        """The per-machine supervisor tree is no longer a standalone
        `executor emit` — serialize-manifest writes it as
        <root>/<machine>/executor.json. Returns that path."""
        jp = Path(root) / machine / "executor.json"
        if not jp.exists():
            raise AssertionError(f"no executor.json slice at {jp}")
        return str(jp)

    @keyword("Executor Json Root Strategy Is")
    def executor_root_strategy(self, path: str, expected: str) -> None:
        """executor.json IS the nested supervisor tree — the document's
        top-level object is the `root` supervisor (name=root + strategy +
        children[]), the exact shape the C++ supervisor's load_manifest
        parses (root must have `children`). Assert root's strategy."""
        import json as _json
        doc = _json.loads(Path(path).read_text())
        if doc.get("name") != "root" or "children" not in doc:
            raise AssertionError(
                f"{path} root is not a supervisor (name={doc.get('name')!r}, "
                f"keys={sorted(doc)})"
            )
        got = doc.get("strategy")
        if got != expected:
            raise AssertionError(
                f"{path} root strategy = {got!r}, expected {expected!r}"
            )

    @keyword("Executor Worker Has Nodes")
    def executor_worker_has_nodes(self, path: str, worker: str,
                                  *node_names: str) -> None:
        """Find a worker leaf by name in the nested tree and assert it
        carries the named .art nodes (tipc metadata). Proves PROCESS_NODES
        flows into the executor.json the supervisor forks against."""
        import json as _json
        doc = _json.loads(Path(path).read_text())

        def _find(n):
            if n.get("name") == worker and n.get("type") == "worker":
                return n
            for c in n.get("children", []):
                hit = _find(c)
                if hit:
                    return hit
            return None

        w = _find(doc)
        if w is None:
            raise AssertionError(f"{path} has no worker leaf {worker!r}")
        got = {nd.get("name") for nd in w.get("nodes", [])}
        missing = [n for n in node_names if n not in got]
        if missing:
            raise AssertionError(
                f"{path} worker {worker!r} nodes = {got}; missing {missing}"
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
        env["PATH"] = f"{self._venv_bin()}:{env.get('PATH', '')}"
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

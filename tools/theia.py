#!/usr/bin/env python3
"""`theia` — the workspace command dispatcher.

Modeled on Mosaic's `moz` (up/mosaic-eng-ref/vehicle_os/mosaic.py): a flat
command map of name → external command, dispatched by argparse-style
matching. Self-contained — no dependencies beyond the stdlib, no click. The
`.art` / codegen surface stays on `artheia`; this wrapper is only for the
workspace-level lifecycle verbs that aren't artheia subcommands.

Commands:

    theia rig up          docker compose up   (the two-host deploy stack)
    theia rig down        docker compose down
    theia provision       ansible-playbook — Phase 1 (os pkgs + Mender client)
    theia orchestrate     ansible-playbook — Phase 2 (app rollout, no restart)
                          (both agentless over SSH)
    theia manifest        rig.py → dist/manifest/*.json (the SOLE rig entry)
    theia dist            per-host .ipk from dist/manifest/ JSON (no rig.py)
    theia install         build + populate install/<machine>/ (local host)
                          + manifests → install/manifest/<machine>/
    theia compdb          regen compile_commands.json from bazel (for clangd)

Extra args after the verb pass through (e.g. `theia rig up central`,
`theia install compute`).
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

# THEIA_ROOT — the framework checkout (whose tools/ holds this script, or
# $THEIA_ROOT when a consuming workspace sourced env.sh / the deb's setup.sh).
# Framework assets
# (deploy/, rules/) resolve against it. This file lives at <root>/tools/theia.py,
# so the checkout root is its parent's parent.
THEIA_ROOT = Path(os.environ.get("THEIA_ROOT") or Path(__file__).resolve().parent.parent)

# WORKSPACE — the directory the user RAN `theia` from. For a consuming workspace
# (gataway_ws, test_ws) that's the workspace itself; the rig, dist/manifest, and
# install/ live HERE, not in the framework. THEIA_INVOCATION_CWD is set by main()
# before it chdir's; falls back to THEIA_ROOT when run inside the framework repo.
# The consuming workspace root. $THEIA_WORKSPACE (set authoritatively by the
# ws's setup_local.sh — it knows its own location) wins; else the invocation cwd
# (the older $PWD-at-source-time path). All deploy DATA (dist/manifest, deploy/
# {registry,config}) roots here, NOT the framework.
WORKSPACE = Path(
    os.environ.get("THEIA_WORKSPACE")
    or os.environ.get("THEIA_INVOCATION_CWD")
    or os.getcwd()
).resolve()
# Running inside the framework checkout itself → workspace IS the framework.
if not (WORKSPACE / "manifest").is_dir() and not (WORKSPACE / ".theia").is_file() \
        and (THEIA_ROOT / "apps").is_dir():
    # No consuming-workspace markers here and the framework tree is under us:
    # treat the framework as the workspace (the in-repo dev path, unchanged).
    if WORKSPACE == THEIA_ROOT or str(WORKSPACE).startswith(str(THEIA_ROOT)):
        WORKSPACE = THEIA_ROOT

COMPOSE = THEIA_ROOT / "deploy" / "docker-compose.yml"

# ── SWP signing material (app-plane authenticity) ────────────────────────────
# The operator generates an RSA keypair with `theia cert generate`; release-swp
# signs the .mender with the PRIVATE key; colony ships the PUBLIC key to each rig
# (`theia cert copy` uploads it to the S3 provisioning plane) where mender.conf's
# ArtifactVerifyKey points at it and mender-update REFUSES anything not signed by
# it. NEITHER key is ever committed — deploy/signing/ is gitignored and the pair
# is regenerated per deployment/nightly (an ephemeral dir via THEIA_SIGNING_DIR).
SIGNING_DIR = Path(os.environ.get("THEIA_SIGNING_DIR")
                   or (WORKSPACE / "deploy" / "signing"))
SWP_SIGN_KEY = SIGNING_DIR / "theia-swp-signing.key"     # PRIVATE (never leaves)
SWP_VERIFY_KEY = SIGNING_DIR / "theia-swp-verify.pub"    # PUBLIC (shipped to rigs)
# Where colony's provision pulls the verify key from, and drops it on the rig.
SWP_VERIFY_S3_KEY = "provisioning/artifact-verify-key.pem"
SWP_VERIFY_RIG_PATH = "/etc/mender/artifact-verify-key.pem"
# provision/orchestrate/cleanup moved OUT to the `colony` repo (the deploy adapter):
# theia's deploy surface is now `manifest`/`dist`/`release` (emit the per-rig
# bundle to S3 — theia release <svc> --s3 ships the runtime debs + manifest+config;
# release-swp ships the app SWP). colony pulls the manifest FROM S3 (no local
# workspace) and resolves the host from Mender — the deploy/registry is GONE
# (registry-free). See docs/tasks/BACKLOG/repo-separation.md. (COMPOSE stays.)


def _run(argv: list[str], cwd: "Path | None" = None) -> int:
    """Run a command (default cwd = WORKSPACE), streaming output; return rc.

    Injects WORKSPACE onto PYTHONPATH so a subprocess (`artheia
    generate-manifest manifest.rig`) can import the workspace's own rig +
    generated manifest modules (manifest.rig imports manifest.apps.manifest). Python
    doesn't put cwd on sys.path for console-script entry points, so we do it.

    `cwd` overrides the working dir — e.g. a framework bazel build runs from
    THEIA_ROOT when the consuming workspace has no MODULE.bazel of its own."""
    print(f"$ {' '.join(argv)}", file=sys.stderr)
    env = os.environ.copy()
    ws = str(WORKSPACE)
    # Build the PYTHONPATH prefix. WORKSPACE lets a subprocess (`artheia
    # generate-manifest manifest.rig`) import the workspace's rig + generated
    # manifest modules. BUT: when the subprocess runs with cwd=WORKSPACE (the
    # framework checkout), Python auto-adds cwd to sys.path, and the source-tree
    # `artheia/` DIRECTORY there shadows the editable-installed `artheia` PACKAGE
    # as a bare namespace portion — so `from artheia import __version__` fails
    # (the real artheia/artheia/__init__.py never runs). Prepend the package's
    # real parent (THEIA_ROOT/artheia) so the installed package wins. Absent in a
    # pip-installed consuming workspace (no source tree) → harmless no-op.
    prefix = [ws]
    # THEIA_ROOT on the path too, for the source-tree standalone services rig
    # (manifest/services/rig.py) which imports `manifest.services.*` relative to
    # the framework checkout. The generated consuming-workspace rig no longer
    # relies on this — it loads the framework's services manifest BY PATH from
    # $THEIA_ROOT/manifest/ (theia.py `_load_services_manifest` in the rig
    # template), so no generic top-level `manifest` package leaks onto the user's
    # PYTHONPATH via the deb. Skipped when ws IS the framework (already on cwd).
    if str(THEIA_ROOT) != ws:
        prefix.append(str(THEIA_ROOT))
    artheia_parent = THEIA_ROOT / "artheia"
    if (artheia_parent / "artheia" / "__init__.py").is_file():
        prefix.insert(0, str(artheia_parent))
    env["PYTHONPATH"] = os.pathsep.join(
        prefix + ([env["PYTHONPATH"]] if env.get("PYTHONPATH") else []))
    try:
        return subprocess.call(argv, env=env,
                               cwd=str(cwd) if cwd is not None else None)
    except FileNotFoundError as e:
        print(f"theia: {e}", file=sys.stderr)
        return 127
    except KeyboardInterrupt:
        return 130


# TIPC address-collision gate. The .art aggregators that, followed recursively,
# union the WIDEST cross-FC node set for the deploy — so a duplicate (type,
# instance) across DIFFERENT FCs (e.g. com's ComDaemon vs per's PerManager both
# claiming 0x80010008) is caught here, before manifests/dist are built. TIPC
# routes purely by address, so a collision is a silent runtime mis-wire.
_ADDRESS_CHECK_ARTS = (
    "system/services/cluster.art",   # all platform FCs (com + per + …)
    "system/system.art",             # demo + supervisor (+ whatever the ws adds)
)


def _check_tipc_addresses() -> int:
    """Run `artheia check-addresses` over the deploy aggregators before any
    manifest/dist generation. Returns 0 if all node TIPC addresses are
    distinct system-wide; non-zero (and prints the clash) otherwise — the
    caller MUST abort generation on a non-zero return."""
    for art in _ADDRESS_CHECK_ARTS:
        if not (WORKSPACE / art).is_file():
            continue                       # aggregator absent in this layout
        if (rc := _run(["artheia", "check-addresses", art])) != 0:
            print(f"theia: TIPC address collision in {art} — fix the .art "
                  "(pick a distinct `tipc type=…`) before generating "
                  "manifests.", file=sys.stderr)
            return rc
    return 0


# --- commands: each takes pass-through args, returns an exit code -----------

def cmd_rig(args: list[str]) -> int:
    """docker compose {up|down|status} the deploy stack.

      theia rig up [args…]     docker compose up -d the stack
      theia rig down [args…]   docker compose down the stack
      theia rig status         the stack's containers + the LIVE cluster (rtdb)
    """
    if not args or args[0] not in ("up", "down", "status"):
        print("usage: theia rig {up|down|status} [compose-args...]",
              file=sys.stderr)
        return 2
    if args[0] == "status":
        return _rig_status(args[1:])
    action, extra = args[0], list(args[1:])
    # `up` detaches by default so the shell returns; pass -d/--detach to keep.
    if action == "up" and not any(a in ("-d", "--detach") for a in extra):
        extra = ["-d", *extra]
    return _run(["docker", "compose", "-f", str(COMPOSE), action, *extra])


def _rig_status(args: list[str]) -> int:
    """`theia rig status` — the deploy stack's containers + the LIVE cluster.

    Two views:
      1. DOCKER: the compose stack's containers (name / state / uptime), from
         `docker compose ps` — so you see which theia-<machine> boards are up.
      2. CLUSTER: the running system pulled from rtdb over com's gRPC (:7700):
         the machines (instance/name/host/present) and a per-machine FC count.
    Best-effort: a view that can't be reached (no docker / com not up yet) is
    reported, not fatal. `--target host:port` overrides com's endpoint for rtdb.
    """
    import json as _json

    target = None
    for i, a in enumerate(args):
        if a == "--target" and i + 1 < len(args):
            target = args[i + 1]

    # ── 1. DOCKER: the compose stack ──────────────────────────────────────────
    print("── docker stack ──────────────────────────────────────────────")
    if not shutil.which("docker"):
        print("  docker not found — skipping the container view")
    else:
        # JSON so we render a stable table regardless of the docker CLI version.
        r = subprocess.run(
            ["docker", "compose", "-f", str(COMPOSE), "ps", "--all",
             "--format", "json"],
            capture_output=True, text=True)
        rows = []
        if r.returncode == 0 and r.stdout.strip():
            # `compose ps --format json` emits either a JSON array or NDJSON
            # (one object per line) depending on the version — handle both.
            txt = r.stdout.strip()
            try:
                rows = _json.loads(txt)
                if isinstance(rows, dict):
                    rows = [rows]
            except _json.JSONDecodeError:
                rows = [_json.loads(ln) for ln in txt.splitlines() if ln.strip()]
        if not rows:
            print("  (no stack containers — `theia rig up` to start them)")
        else:
            print(f"  {'CONTAINER':<20} {'STATE':<10} {'STATUS'}")
            for c in rows:
                name = c.get("Name") or c.get("name") or "?"
                state = c.get("State") or c.get("state") or "?"
                status = c.get("Status") or c.get("status") or ""
                print(f"  {name:<20} {state:<10} {status}")

    # ── 2. CLUSTER: the live system via rtdb (com gRPC :7700) ─────────────────
    print("\n── live cluster (rtdb → com :7700) ───────────────────────────")
    rtdb = shutil.which("rtdb") or str(WORKSPACE / ".venv" / "bin" / "rtdb")
    if not Path(rtdb).exists():
        print("  rtdb not on PATH / .venv — skipping the cluster view")
        return 0

    def _rtdb(sub: list[str]) -> "tuple[int, str]":
        cmd = [rtdb, *(["--target", target] if target else []), *sub]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
        return p.returncode, (p.stdout or p.stderr)

    try:
        rc, out = _rtdb(["machines"])
    except Exception as e:  # noqa: BLE001 — com not up / no route
        print(f"  com not reachable ({e}) — is the stack up + com listening?")
        return 0
    if rc != 0 or "INST" not in out:
        print("  com not reachable — is the stack up + com listening on :7700?")
        if out.strip():
            print("  " + out.strip().splitlines()[-1])
        return 0
    # Echo the machines table (rtdb already formats INST/MACHINE/PRESENT/HOST).
    for ln in out.strip().splitlines():
        print("  " + ln)

    # Per-machine FC count. Pull the AGGREGATE `ps` ONCE and bucket the rows by
    # their machine-name prefix (com name-prefixes every node in the merged tree:
    # "master/com", "compute/shwa", …). This is robust regardless of whether a
    # per-machine selector resolves on this stack — one call, correct counts.
    try:
        _rc, pout = _rtdb(["ps"])
    except Exception:  # noqa: BLE001
        return 0
    counts: "dict[str, int]" = {}
    for ln in pout.strip().splitlines()[1:]:          # skip the header row
        # the NAME column carries "<machine>/<fc>"; find it (the token with a '/').
        tok = next((t for t in ln.split() if "/" in t), None)
        if tok:
            counts[tok.split("/", 1)[0]] = counts.get(tok.split("/", 1)[0], 0) + 1
    if counts:
        print("\n  FCs per machine:")
        for name in sorted(counts):
            print(f"    {name:<12} {counts[name]} FC(s)")
    return 0



def _bazel_root() -> Path:
    """The dir bazel builds run in. A consuming workspace with its OWN
    MODULE.bazel (e.g. gataway_ws → @pero_theia) builds in place; an empty /
    no-bazel workspace builds the framework targets (//platform/...) from
    THEIA_ROOT (the framework module). So: WORKSPACE if it has MODULE.bazel,
    else THEIA_ROOT."""
    return WORKSPACE if (WORKSPACE / "MODULE.bazel").is_file() else THEIA_ROOT


def _is_framework_target(target: str) -> bool:
    """A bazel label OWNED by the framework (built in THEIA_ROOT), vs the
    workspace's own app targets. //platform/... and //services/... are the
    framework; //apps/... (and anything else) is the workspace's."""
    pkg = target.lstrip("/").split(":", 1)[0]
    return pkg.startswith(("platform/", "services/"))


def _bazel_bin(target: str) -> Path:
    """//pkg/dir:name -> <root>/bazel-bin/pkg/dir/name. Framework targets read
    from THEIA_ROOT's bazel-bin; the workspace's own app targets from WORKSPACE's
    (or THEIA_ROOT's when the workspace has no MODULE.bazel)."""
    pkg, name = target.lstrip("/").split(":", 1)
    if _is_framework_target(target):
        root = THEIA_ROOT
    else:
        root = WORKSPACE if (WORKSPACE / "MODULE.bazel").is_file() else THEIA_ROOT
    return root / "bazel-bin" / pkg / name


def _pero_theia_root() -> "Path | None":
    """Resolve the local path for the `pero_theia` bazel module from the
    workspace MODULE.bazel's `local_path_override`. Returns None when the
    workspace has no MODULE.bazel or no such override (registry dep)."""
    mod = WORKSPACE / "MODULE.bazel"
    if not mod.is_file():
        return None
    import re as _re
    text = mod.read_text()
    m = _re.search(
        r'local_path_override\s*\(\s*module_name\s*=\s*"pero_theia"\s*,\s*'
        r'path\s*=\s*"([^"]+)"', text)
    if not m:
        return None
    p = Path(m.group(1))
    return p if p.is_absolute() else (WORKSPACE / p).resolve()


def _deb_mode() -> bool:
    """True when the pero_theia bazel module (or THEIA_ROOT) is an INSTALLED
    prefix (the debs), not a full source checkout.

    Two cases:
    - THEIA_ROOT has a prebuilt supervisor under bin/ → installed deb, no source.
    - The workspace MODULE.bazel has a local_path_override for pero_theia that
      points to an installed prefix (lacks platform/supervisor/main/BUILD.bazel).
      This is the consuming-workspace-alongside-source-checkout case: THEIA_ROOT
      points to the source tree but @pero_theia is the deb at /opt/theia."""
    if (THEIA_ROOT / "bin" / "supervisor").is_file():
        return True
    proot = _pero_theia_root()
    if proot and not (proot / "platform" / "supervisor" / "main").is_dir():
        return True
    return False


def _prebuilt_bin(name: str) -> "Path | None":
    """The prebuilt framework binary <name> under the installed prefix
    (THEIA_ROOT/bin/<name>), or None if absent. The deb-mode source for the
    supervisor + the ARA service FCs (com/per/sm/ucm/log/shwa)."""
    p = THEIA_ROOT / "bin" / name
    return p if p.is_file() else None


def _discover_rig_module(zonal: bool = False) -> "str | None":
    """Find the workspace's rig Python module — name-INDEPENDENT, so Theia works
    in the monorepo (apps.manifest.rig) AND a downstream workspace (manifest.rig)
    with no hardcoded name.

    Resolution order:
      1. $THEIA_RIG_MODULE (zonal: $THEIA_ZONAL_RIG_MODULE) — explicit override.
      2. The single `*/manifest/rig.py` (or zonal_rig.py) under the workspace
         → its dotted module (apps/manifest/rig.py → apps.manifest.rig;
         manifest/rig.py → manifest.rig).
    Returns None if none/ambiguous (caller errors helpfully).

    The "zonal" (2-machine) rig has TWO flavours: test_rig.py (all-x86, for the
    docker provisioning/orchestration test — the in-repo default) and
    zonal_rig.py (arm64 rpi4+jetson, real hardware). `theia manifest`/`dist` for
    docker want the x86 test_rig, so it's PREFERRED here; zonal_rig (hardware) is
    reached by an explicit module arg or $THEIA_ZONAL_RIG_MODULE."""
    env = os.environ.get(
        "THEIA_ZONAL_RIG_MODULE" if zonal else "THEIA_RIG_MODULE")
    if env:
        return env
    # 2-machine: prefer the x86 docker test_rig, fall back to the arm64 zonal_rig.
    leaves = ["test_rig.py", "zonal_rig.py"] if zonal else ["rig.py"]
    # Trees that are not the WORKSPACE's deploy rig: vendored repos, the venv,
    # bazel/build outputs, the framework's own internals (artheia.manifest.*),
    # and the shipped workspace template.
    _skip = {"up", ".venv", "bazel-bin", "bazel-out", "external", "vendor",
             "artheia", "templates", "build", ".git"}
    # Try each candidate leaf in PREFERENCE order; return the first that resolves
    # to exactly one module (test_rig before zonal_rig for the 2-machine case).
    for leaf in leaves:
        hits = []
        for p in WORKSPACE.glob(f"**/manifest/{leaf}"):
            rel = p.relative_to(WORKSPACE)
            if any(seg in _skip for seg in rel.parts):
                continue
            # dotted module = the path minus the .py, dirs joined by '.'
            hits.append(".".join(rel.with_suffix("").parts))
        if len(hits) == 1:
            return hits[0]
        if len(hits) > 1:
            # Ambiguous: prefer a top-level `manifest.<leaf>` (downstream conv).
            top = f"manifest.{leaf[:-3]}"
            if top in hits:
                return top
    return None


#: The framework supervisor runtime binary. It is the platform runtime, NOT a
#: process in any rig's manifest — `serialize-manifest` never emits it — so the
#: install path carries it as a fixed target. It is built/staged separately and
#: lands at install/<machine>/supervisor (not bin/).
_SUPERVISOR_TARGET = "//platform/supervisor/main:supervisor"


def _load_install_components(manifest_root: Path, machine: str):
    """Read the generated AUTOSAR execution.json and return what to build/stage.

    The deploy MANIFEST is the single source of truth — no hardcoded binary
    list. `artheia serialize-manifest` writes
    ``<manifest_root>/<machine>/execution.json`` whose ``processes[]`` each carry
    ``name`` (the staged ``bin/<name>`` leaf), ``executable`` (the bazel label),
    ``machine`` and ``start_cmd`` (``bin/<name>``).

    Returns ``(supervisor_target, {name: target})`` over the processes ON THIS
    machine (install stages one machine at a time). The supervisor is the fixed
    framework runtime (`_SUPERVISOR_TARGET`) — it is not in the manifest, so it's
    returned split out (it lands at ``<dest>/supervisor``, not ``bin/``).

    Returns ``(_SUPERVISOR_TARGET, {})`` for a host/admin machine (no processes
    on it) — the caller stages just machines.json, no supervisor tree. Raises if
    the per-machine execution.json is missing."""
    import json

    execjson = manifest_root / machine / "execution.json"
    if not execjson.is_file():
        raise FileNotFoundError(
            f"no execution.json at {execjson} — "
            f"`artheia serialize-manifest` must run first")
    data = json.loads(execjson.read_text())
    binaries: dict[str, str] = {}
    for p in data.get("processes", []):
        if p.get("machine") != machine:
            continue
        if not p.get("bazel_buildable", True):
            continue
        name, target = p.get("name"), p.get("executable")
        if not name or not target:
            continue
        binaries[name] = target
    return _SUPERVISOR_TARGET, binaries


def _fc_art_path(fc: str, target: str):
    """The .art the gen-params emitter reads for an FC.

    Services FCs (``//services/<fc>/...``) live at the canonical symlink path
    system/services/<fc>/package.art — the FC name IS the dir segment.

    App compositions are resolved from ``system/system.art`` imports: we walk
    each imported package dir, look for a ``package.art``, and check whether
    that package's cluster members include *fc* as an ident. This is
    layout-independent — the user can name their app dir anything; artheia's
    ``_import_dir`` follows the package FQN to the right dir regardless.

    Returns a Path or None if no art is found (platform FCs carry their own)."""
    if target.startswith("//services/"):
        cand = WORKSPACE / "system" / "services" / fc / "package.art"
        return cand if cand.exists() else None

    if not target.startswith("//apps/"):
        return None   # platform FCs (gateway) carry their own params path

    # Resolve via system/system.art imports — layout-independent. The user
    # can name their app dir anything; artheia's _import_dir follows the
    # package FQN declared in system.art to the right dir regardless.
    system_art = WORKSPACE / "system" / "system.art"
    if not system_art.exists():
        return None
    try:
        from artheia.generators._art_clusters import (
            _extract_package, _extract_imports, _import_dir,
            _PKG_FILE_PRIORITY,
        )
        from artheia.model.loader import parse_file
    except ImportError:
        return None
    entry_pkg = _extract_package(str(system_art))
    for imp_pkg in _extract_imports(str(system_art)):
        pkg_dir = _import_dir(system_art, entry_pkg, imp_pkg)
        if pkg_dir is None or not pkg_dir.is_dir():
            continue
        for fname in _PKG_FILE_PRIORITY:
            cand = pkg_dir / fname
            if not cand.is_file():
                continue
            # Parse package (merges package.art + component.art) and scan
            # ClusterDecl.elements for a ClusterMember whose ident == fc.
            try:
                m = parse_file(str(cand))
                for el in getattr(m, "elements", []):
                    if type(el).__name__ == "ClusterDecl":
                        for member in getattr(el, "elements", []):
                            if getattr(member, "name", None) == fc:
                                return cand
            except Exception:
                pass
            break  # one file per pkg dir; move on to next import
    return None


def _install_dir(args: list[str]) -> Path:
    """install/<machine>/ — the machine from the arg, else $THEIA_MACHINE, else
    the single staged machine under install/ (a single-host workspace needs no
    name), else 'central'."""
    machine = next((a for a in args if not a.startswith("-")), None)
    if machine is None:
        machine = os.environ.get("THEIA_MACHINE")
    if machine is None:
        root = WORKSPACE / "install"
        staged = [d.name for d in root.iterdir()
                  if (d / "supervisor").is_file()] if root.is_dir() else []
        machine = staged[0] if len(staged) == 1 else "central"
    return WORKSPACE / "install" / machine


def _sup_pidfile(dest: Path) -> Path:
    return dest / "supervisor.pid"


def _tipc_addr_bound(tipc_type: int, instance: int) -> bool:
    """True if a TIPC name (type, instance) is currently published on this host.

    Reads `tipc nametable show` and checks for any row whose Type matches and
    whose [Lower, Upper] range covers `instance`. Used by `theia start` to detect
    a cross-workspace supervisor already bound at the same address (the pidfile
    guard is per-workspace and can't see it). Best-effort: if `tipc` is missing or
    errors, returns False (don't block a start on a tooling gap)."""
    try:
        out = subprocess.run(["tipc", "nametable", "show"],
                             capture_output=True, text=True, timeout=5).stdout
    except (FileNotFoundError, subprocess.SubprocessError):
        return False
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3 or not parts[0].isdigit():
            continue          # header / malformed
        try:
            row_type, lower, upper = int(parts[0]), int(parts[1]), int(parts[2])
        except ValueError:
            continue
        if row_type == tipc_type and lower <= instance <= upper:
            return True
    return False


def cmd_start(args: list[str]) -> int:
    """Run the staged supervisor from install/<machine>/ (default central).

    The supervisor forks every child from executor.json; bring the whole stack
    up with one verb instead of the hand-typed
    `THEIA_SUPERVISOR_MANIFEST=… ./supervisor`. Detached (setsid) with a pidfile
    so `theia stop` can bring it down gracefully — and so it survives the
    calling shell (the supervisor must outlive `theia start`). Logs stream to
    install/<machine>/supervisor.log. Idempotent: refuses if one is already up.

    Pass an INSTANCE via `--instance N` (THEIA_SUPERVISOR_INSTANCE, default 0)."""
    if "-h" in args or "--help" in args:
        print(cmd_start.__doc__, file=sys.stderr)
        return 0
    dest = _install_dir(args)
    sup = dest / "supervisor"
    if not sup.is_file():
        print(f"theia: {sup} not found — run `theia install` first.",
              file=sys.stderr)
        return 1

    pidfile = _sup_pidfile(dest)
    if pidfile.is_file():
        try:
            old = int(pidfile.read_text().strip())
            os.kill(old, 0)  # still alive?
            print(f"theia: supervisor already running (pid {old}); "
                  f"`theia stop` first.", file=sys.stderr)
            return 1
        except (ValueError, ProcessLookupError, PermissionError):
            pidfile.unlink(missing_ok=True)  # stale

    instance = "0"
    for i, a in enumerate(args):
        if a == "--instance" and i + 1 < len(args):
            instance = args[i + 1]

    # Machine instance = the TIPC instance every node on this machine binds
    # (central=0, compute=1, …). Resolve it from this machine's machine.json
    # (Machine.machine_index), so the supervisor binds its ctl at supervisor:N and
    # sets THEIA_NODE_TIPC per child shifted to N. An explicit --instance arg wins;
    # else machine.json's index; else 0 (single-machine / legacy — unchanged).
    machine_instance = instance
    if instance == "0":  # no explicit override → look up the manifest
        mname = dest.name
        for mj in (WORKSPACE / "install" / "manifest" / mname / "machine.json",
                   dest / "machine.json"):
            try:
                import json as _json
                _m = _json.loads(mj.read_text())
                idx = (_m.get("machine") or _m).get("machine_index")
                if idx is not None:
                    machine_instance = str(int(idx))
                    break
            except (OSError, ValueError, KeyError, TypeError):
                continue

    # FAIL-FAST on a cross-workspace TIPC address collision. The supervisor's ctl
    # binds a FIXED TIPC address 0x80020001:<machine_instance>. The pidfile guard
    # above only sees THIS workspace's supervisor — a supervisor from a DIFFERENT
    # workspace (its own pidfile) that co-binds the same address is invisible to
    # it. Since TIPC name binds are SEQPACKET-anycast, two co-bound supervisors
    # make a probe (tdb ps / GetTree) land nondeterministically on either one (or
    # time out if one is crash-looping). One host = one supervisor per address:
    # distinct dev stacks must use distinct tipc_cluster_id (or a distinct
    # machine_index). Refuse to start rather than silently co-bind.
    _sup_bound = _tipc_addr_bound(0x80020001, int(machine_instance))
    if _sup_bound:
        print(f"theia: a supervisor is ALREADY bound at TIPC 0x80020001:"
              f"{machine_instance} on this host (not from this workspace — its "
              f"pidfile is separate).\n"
              f"  Two supervisors sharing one TIPC address anycast-collide: "
              f"`tdb ps` / probes hit the wrong one or time out.\n"
              f"  Stop the other stack (`theia stop` in its workspace), or give "
              f"this rig a distinct Machine.tipc_cluster_id / machine_index.",
              file=sys.stderr)
        return 1

    log = dest / "supervisor.log"
    # THEIA_INSTALL_DIR: colon-separated list of dirs the supervisor scans to
    # resolve each child's relative start_cmd (e.g. "bin/crypto"). First match
    # wins. In deb mode the framework FCs (com/per/etc.) are under /opt/theia/bin,
    # and the local user app bins are under <dest>/current/bin — list both so a
    # mixed rig (deb framework + local app) works without a `current` flip.
    # In source mode everything was staged to <dest>/current/bin by theia install.
    _dest_abs = str(dest.resolve())
    _install_dirs = _dest_abs + "/current"
    if _deb_mode():
        _install_dirs = str(THEIA_ROOT.resolve()) + ":" + _install_dirs
    env = {
        **os.environ,
        "THEIA_SUPERVISOR_MANIFEST": "config/executor.json",
        "THEIA_INSTALL_DIR": _install_dirs,
        # The deploy root the children anchor their config/ to. A forked FC runs
        # with cwd=<dest>/releases/local (the release symlink), so its
        # init_config("<fc>") + THEIA_CONFIG_DIR=config (relative) would look under
        # releases/local/config/ — which does NOT exist (config/ is at <dest>/
        # config/). init_config anchors a RELATIVE THEIA_CONFIG_DIR to
        # THEIA_ROOT_DIR (mirrors platform/runtime/ota/theia-run.sh on a real rig,
        # where it's /opt/theia). Without this, a node's per-rig params (e.g.
        # <node>.enabled=false) silently fall back to their .art defaults.
        "THEIA_ROOT_DIR": _dest_abs,
        "THEIA_SUPERVISOR_INSTANCE": instance,
        # The cluster machine index — a supervisor BOOT knob (not a node address):
        # the supervisor reads it to shift each CHILD's --tipc instance by N when
        # it builds the child's argv. The supervisor's OWN ctl address is passed
        # via --tipc below (ARG-only, like every node).
        "THEIA_MACHINE_INSTANCE": machine_instance,
    }
    # The cluster manifest ROOT (machines.json + per-machine machine.json). The
    # supervisor passes it down to every child it forks; com reads it to map a
    # TIPC instance back to a machine NAME (central/compute) for per-machine
    # telemetry labels. Only set when a cluster manifest exists next to the
    # install (a single-machine dev stack has none → com falls back to "mN").
    for _mroot in (WORKSPACE / "install" / "manifest", dest.parent):
        if (_mroot / "machines.json").is_file():
            env["THEIA_MACHINE_MANIFEST"] = str(_mroot)
            break
    # mTLS opt-in (mirrors platform/runtime/ota/theia-run.sh): when this machine's certs
    # are staged (install/<machine>/certs or dist/manifest/<machine>/certs),
    # export THEIA_COM_TLS_* so the forked com children SERVE mutual TLS. The
    # supervisor inherits + passes it down. No certs → com stays plaintext (the
    # local dev default). `theia observer` REQUIRES these, so staging certs is
    # how you flip the whole loop (com server + GUI/rtdb clients) to TLS.
    for _cdir in (dest / "certs", WORKSPACE / "dist" / "manifest" / dest.name / "certs"):
        if (_cdir / "server.crt").is_file() and (_cdir / "server.key").is_file():
            env["THEIA_COM_TLS_CERT"] = str(_cdir / "server.crt")
            env["THEIA_COM_TLS_KEY"] = str(_cdir / "server.key")
            if (_cdir / "ca.crt").is_file():
                env["THEIA_COM_TLS_CA"] = str(_cdir / "ca.crt")
            print(f"theia start: mTLS on — com serves TLS from {_cdir}",
                  file=sys.stderr)
            break
    # Bundled shared libs the FCs link at runtime (per → libetcd-cpp-api.so). In
    # sibling-source mode the .so lives under THEIA_ROOT (not on the loader path
    # + the bazel binary's RPATH breaks once staged to install/bin/); in deb mode
    # /opt/theia/lib is ldconfig'd already but adding it is harmless. Prepend so
    # the forked children find them without a manual `export LD_LIBRARY_PATH`.
    _libdirs = [p for p in (
        THEIA_ROOT / "third_party" / "etcd-cpp-apiv3" / "install" / "lib",
        Path("/opt/theia/lib"),
    ) if p.is_dir()]
    if _libdirs:
        prev = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = os.pathsep.join(
            [str(p) for p in _libdirs] + ([prev] if prev else []))
    # Detach into its own session so it outlives this process; redirect output
    # to the log. start_new_session=True == setsid → the supervisor leads its
    # own session (its children already setsid per-worker).
    logf = open(log, "ab")
    # The supervisor's own SupervisorCtl address is ARG-driven like every node:
    # --tipc=supervisor_ctl=<type>:<machine_index>. Instance-only (":N") keeps the
    # compiled type 0x80020001. central → :0, compute → :1.
    sup_argv = ["./supervisor"]
    if machine_instance != "0":
        sup_argv.append(f"--tipc=supervisor_ctl=:{machine_instance}")
    proc = subprocess.Popen(
        sup_argv, cwd=str(dest), env=env,
        stdout=logf, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
        start_new_session=True,
    )
    pidfile.write_text(str(proc.pid) + "\n")
    print(f"theia: supervisor up (pid {proc.pid}, instance {instance}, "
          f"machine {machine_instance}) — log: {log}", file=sys.stderr)
    # Brief liveness check: if it died immediately (bad manifest / port in use),
    # surface the tail rather than leave a stale pidfile.
    import time
    time.sleep(1.0)
    if proc.poll() is not None:
        pidfile.unlink(missing_ok=True)
        print(f"theia: supervisor exited immediately (rc={proc.returncode}); "
              f"tail of {log}:", file=sys.stderr)
        _run(["tail", "-n", "15", str(log)])
        return 1

    # First-boot config seed: populate /theia/config/* from the demo's DECLARED
    # config defaults, for any node that has no stored value yet. Idempotent —
    # safe to run on every start (already-stored keys are left untouched), and
    # best-effort: a seed failure (no etcd, per still coming up) is a warning,
    # not a start failure. Skip with --no-seed.
    if "--no-seed" not in args:
        _seed_defaults()

    return 0


def _seed_defaults() -> None:
    """Seed declared config defaults into etcd via services/per (first-boot).

    Generates the schema + config-defaults from system/apps/package.art and runs
    migration/seed.py's idempotent `defaults` action. per must be up and reachable
    (give it a moment after the supervisor forks it). Best-effort: any failure is
    logged, never fatal."""
    import tempfile
    import time

    import json as _json

    # Seed EVERY config-bearing node's declared defaults into etcd — not just the
    # demo nodes. gen-config-defaults/gen-schema resolve compositions per-PACKAGE
    # (the cluster forward-decl stubs in system/system.art carry no prototypes),
    # so we run them over EACH FC's package.art (canonical symlink path — needed
    # for cross-package imports) + the demo, then MERGE. gen-config-defaults emits
    # art_package+proto_type per config so seed.py resolves each FC's proto class
    # dynamically via the probe codec. (Was a single system/apps/package.art →
    # FwConfig/PhmConfig/NmConfig/… never reached etcd.)
    arts = sorted((WORKSPACE / "system" / "services").glob("*/package.art"))
    demo_art = WORKSPACE / "system" / "demo" / "package.art"
    if demo_art.exists():
        arts.append(demo_art)
    if not arts:
        return
    tmp = Path(tempfile.gettempdir())
    schema = tmp / "theia_seed_schema.json"
    defs   = tmp / "theia_seed_defaults.json"

    def _merge_gen(verb: str, out: Path) -> bool:
        """Run `artheia <verb>` over every FC art + demo, merging the per-art
        `configs` dicts into one {package, configs} file. A single art failing is
        skipped (best-effort), not fatal."""
        merged = {"package": "system", "configs": {}}
        any_ok = False
        per_art = tmp / f"_seed_{verb}_part.json"
        for a in arts:
            if _run(["artheia", verb, str(a), "--out", str(per_art)]) != 0:
                continue
            try:
                part = _json.loads(per_art.read_text())
            except Exception:  # noqa: BLE001
                continue
            merged["configs"].update(part.get("configs", {}))
            any_ok = True
        if any_ok:
            out.write_text(_json.dumps(merged, indent=2) + "\n")
        return any_ok

    try:
        if not _merge_gen("gen-schema", schema):
            print("theia: seed skipped — gen-schema failed.", file=sys.stderr)
            return
        if not _merge_gen("gen-config-defaults", defs):
            print("theia: seed skipped — gen-config-defaults failed.",
                  file=sys.stderr)
            return
        # per is forked by the supervisor and binds its TIPC port a few seconds
        # in (after the etcd connect). Retry the (idempotent) seed until it lands
        # or a ~20s budget runs out — the ConnectionError on a not-yet-bound per
        # is the expected early miss, not a real failure.
        seed = [sys.executable, str(WORKSPACE / "migration" / "seed.py"),
                "defaults", "--defaults", str(defs), "--schema", str(schema)]
        for attempt in range(7):
            time.sleep(3.0)
            if subprocess.call(seed, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL) == 0:
                print("theia: config defaults seeded (idempotent).",
                      file=sys.stderr)
                return
        print("theia: config-defaults seed did not land within ~20s "
              "(per slow to start?) — non-fatal; run `migration/seed.py "
              "defaults` manually.", file=sys.stderr)
    except Exception as e:  # noqa: BLE001 — best-effort, never fail the start
        print(f"theia: config-defaults seed errored ({e}) — non-fatal.",
              file=sys.stderr)


def cmd_stop(args: list[str]) -> int:
    """Stop the supervisor started by `theia start` (graceful SIGTERM).

    SIGTERM triggers the supervisor's shutdown_subtree() — it SIGTERMs→reaps→
    SIGKILLs each child's process group, then exits. Children also carry
    PR_SET_PDEATHSIG, so even a hard kill wouldn't orphan them. Reads the
    pidfile; waits up to ~10s for a clean exit before escalating to SIGKILL."""
    import signal
    import time

    if "-h" in args or "--help" in args:
        print(cmd_stop.__doc__, file=sys.stderr)
        return 0
    dest = _install_dir(args)
    pidfile = _sup_pidfile(dest)
    if not pidfile.is_file():
        print(f"theia: no pidfile at {pidfile} — supervisor not started by "
              f"`theia start` (or already stopped).", file=sys.stderr)
        return 0
    try:
        pid = int(pidfile.read_text().strip())
    except ValueError:
        pidfile.unlink(missing_ok=True)
        return 0

    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        pidfile.unlink(missing_ok=True)
        print("theia: supervisor already gone.", file=sys.stderr)
        return 0
    print(f"theia: SIGTERM → supervisor {pid}; waiting for clean shutdown…",
          file=sys.stderr)
    # The supervisor shuts children down SEQUENTIALLY, each up to its own
    # SIGTERM→SIGKILL grace window. Most FCs now exit promptly (interruptible
    # heartbeat); com/per still consume their ~5s grace on external-resource
    # teardown (gRPC server / etcd client), so a full tree is ~12s. 20s budget
    # covers it with margin before escalating.
    for _ in range(200):  # 200 × 0.1s = 20s
        time.sleep(0.1)
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            pidfile.unlink(missing_ok=True)
            print("theia: supervisor stopped.", file=sys.stderr)
            return 0
    # Didn't exit in time — escalate.
    print("theia: supervisor didn't exit in 20s; SIGKILL.", file=sys.stderr)
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    pidfile.unlink(missing_ok=True)
    return 0


def cmd_clean(args: list[str]) -> int:
    """Remove install artifacts and optionally run bazel clean.

        theia clean [--bazel] [--all]

    Symmetric counterpart to `theia install`:

      install/             — remove the entire staged tree (all machines)
      install/manifest/    — remove the serialized per-machine manifests
      dist/manifest/       — remove the dist manifest output (theia manifest)

    Flags:
      --bazel   also run `bazel clean` in both THEIA_ROOT and WORKSPACE
      --all     same as --bazel (convenience alias)
      -h/--help show this message

    Does NOT stop a running supervisor — run `theia stop` first if needed.
    Safe to run on a workspace with no install/ dir (no-op, exit 0)."""
    if "-h" in args or "--help" in args:
        print(cmd_clean.__doc__, file=sys.stderr)
        return 0

    run_bazel = "--bazel" in args or "--all" in args

    removed = []
    for rel in ("install", "dist/manifest"):
        p = WORKSPACE / rel
        if p.exists():
            shutil.rmtree(p)
            removed.append(str(p))

    if removed:
        print(f"theia clean: removed {', '.join(removed)}", file=sys.stderr)
    else:
        print("theia clean: nothing to remove.", file=sys.stderr)

    if run_bazel:
        roots = [THEIA_ROOT]
        if WORKSPACE != THEIA_ROOT and (WORKSPACE / "MODULE.bazel").is_file():
            roots.append(WORKSPACE)
        for root in roots:
            if (rc := _run(["bazel", "clean"], cwd=root)) != 0:
                return rc

    return 0


# The wx supervisor-GUI binary. Bazel lays it out under a per-target bin/ dir.
_OBSERVER_TARGET = "//tools/supervisor-gui:supervisor-gui"


def _client_certs_dir() -> "Path | None":
    """The mTLS material `theia manifest` stages for the local cluster:
    <ws>/dist/manifest/<machine>/certs (preferred) or the install slice
    <ws>/install/<machine>/certs. Returns the dir holding ca.crt + client.{crt,
    key}, preferring 'central', else the first machine dir with a ca.crt; None
    if no dev certs exist. Mirrors rtdb's _default_certs_dir so the GUI + rtdb
    pick the SAME identity."""
    for base in (WORKSPACE / "dist" / "manifest", WORKSPACE / "install"):
        if not base.is_dir():
            continue
        names = ["central", *sorted(p.name for p in base.iterdir() if p.is_dir())]
        for name in names:
            d = base / name / "certs"
            if (d / "ca.crt").is_file():
                return d
    return None


def _probe_send(args: list[str], *, is_call: bool) -> int:
    """Shared impl for `theia cast` / `theia call` — send a message to a node from
    a JSON payload, over TIPC via artheia.probe. Addressing:
      theia {cast|call} <node> <msg_or_op> [--data '{...}'] [--instance N]
                                           [--machine M] [--timeout S]
    <node>/<msg_or_op> resolve against the workspace's system/system.art (which
    imports the app's nodes). --data is a JSON object of the message fields.
    --instance targets a specific CLONE (same TIPC type, that instance) — e.g. one
    of the 10 Counters. --machine shifts the instance by the machine index (the
    per-board clone). A cast with NO --instance is TIPC round-robined across the
    node type's bound ports (the multiplicity demo).
    """
    import json

    def _opt(name, default=None):
        for i, a in enumerate(args):
            if a == name and i + 1 < len(args):
                return args[i + 1]
        return default

    pos = [a for a in args if not a.startswith("-")]
    # skip the values consumed by value-opts so they aren't mistaken for positionals
    _vals = {_opt("--data"), _opt("--instance"), _opt("--machine"), _opt("--timeout")}
    pos = [a for a in pos if a not in _vals]
    if len(pos) < 2:
        verb = "call" if is_call else "cast"
        print(f"usage: theia {verb} <node> <{'op' if is_call else 'msg_type'}> "
              "[--data '{json}'] [--instance N] [--machine M]", file=sys.stderr)
        return 2
    node, msg = pos[0], pos[1]
    data_raw = _opt("--data", "{}")
    try:
        fields = json.loads(data_raw)
        if not isinstance(fields, dict):
            raise ValueError("must be a JSON object")
    except (json.JSONDecodeError, ValueError) as e:
        print(f"theia {'call' if is_call else 'cast'}: bad --data JSON: {e}",
              file=sys.stderr)
        return 2
    instance = _opt("--instance")
    machine = _opt("--machine")
    timeout = float(_opt("--timeout", "2.0"))

    proto_dir = WORKSPACE / "proto"
    try:
        sys.path.insert(0, str(THEIA_ROOT / "artheia"))
        from artheia.gen_server.probe import ArtheiaContext  # noqa: PLC0415
    except Exception as e:  # noqa: BLE001
        print(f"theia: artheia.probe unavailable ({e})", file=sys.stderr)
        return 1
    # Find an .art that DEFINES <node> (carries its tipc address). The probe
    # context resolves nodes from the .art it's given; system.art only IMPORTS the
    # app packages (its own elements are clusters, not nodes), so a node lives in
    # its package's component.art / package.art. Try system.art first (single-node
    # apps whose node is inline), then every component.art / package.art under
    # system/ until one resolves <node>.
    # NOTE: walk with followlinks — a package-consuming workspace maps imported
    # packages into system/ as SYMLINKED dirs (system/<pkg> → the package repo),
    # which Path.glob("**") silently skips; without this, `theia call` cannot
    # resolve any imported-package node. Dedupe by resolved path (a framework
    # symlink tree can alias the same .art twice).
    candidates = [WORKSPACE / "system" / "system.art"]
    _seen: set[str] = set()
    _found: list[Path] = []
    for _root, _dirs, _files in os.walk(WORKSPACE / "system", followlinks=True):
        for _f in sorted(_files):
            if _f in ("component.art", "package.art"):
                _p = Path(_root) / _f
                _r = str(_p.resolve())
                if _r not in _seen:
                    _seen.add(_r)
                    _found.append(_p)
    candidates += sorted(_found, key=lambda q: (q.name != "component.art", str(q)))
    ctx = None
    for art in candidates:
        if not art.exists():
            continue
        # The node's wire types live in the DEFINING package's repo: for an
        # imported package (system/<pkg> symlink → the package repo) the .proto
        # is under THAT repo's proto/, not this workspace's. Anchor proto_root
        # to the resolved .art's own repo (the dir with MODULE.bazel above it),
        # falling back to this workspace's proto/ for local apps.
        _aroot = art.resolve().parent
        while _aroot != _aroot.parent and not (_aroot / "MODULE.bazel").exists():
            _aroot = _aroot.parent
        _proot = _aroot / "proto"
        _pr = _proot if _proot.is_dir() else proto_dir
        try:
            c = ArtheiaContext(str(art), proto_root=str(_pr))
            c.ref(node)          # raises if this .art doesn't define <node>
            ctx = c
            break
        except Exception:  # noqa: BLE001
            continue
    if ctx is None:
        print(f"theia {'call' if is_call else 'cast'}: no .art under "
              f"{WORKSPACE}/system defines node '{node}'.", file=sys.stderr)
        return 1
    import dataclasses
    import os as _os
    # A generic sender probe at a scratch EXTERNAL address (not a .art node). Unique
    # per invocation (pid-derived) so concurrent `theia cast`es don't collide on the
    # reply address.
    probe = ctx.probe_external(0x800100FE, (_os.getpid() & 0x3FFF) + 0x100,
                               name="theia_cli").start()
    try:
        # Resolve the target node ref; apply --instance / --machine as an instance
        # override (the machine shift == machine index added to the base instance).
        tref = ctx.ref(node)
        inst = None
        if instance is not None:
            inst = int(instance)
        if machine is not None:
            base = tref.tipc_instance if inst is None else inst
            inst = base + int(machine)
        target = (dataclasses.replace(tref, tipc_instance=inst)
                  if inst is not None else node)
        where = f" @instance {inst}" if inst is not None else " (round-robin)"
        if is_call:
            print(f"theia call: {node}.{msg}({fields}){where}", file=sys.stderr)
            reply = probe.call(target, msg, timeout=timeout, **fields)
            print(json.dumps(reply, default=str, indent=2))
        else:
            print(f"theia cast: {node} <- {msg}({fields}){where}", file=sys.stderr)
            probe.cast(target, msg, **fields)
            print("cast sent.")
        return 0
    except Exception as e:  # noqa: BLE001
        print(f"theia {'call' if is_call else 'cast'}: {e}", file=sys.stderr)
        return 1
    finally:
        probe.stop()


def cmd_cast(args: list[str]) -> int:
    """cast a message to a node from JSON (test/demo). See _probe_send."""
    if "-h" in args or "--help" in args:
        print(_probe_send.__doc__, file=sys.stderr)
        return 0
    return _probe_send(args, is_call=False)


def cmd_call(args: list[str]) -> int:
    """call a node operation from JSON, print the reply (test/demo)."""
    if "-h" in args or "--help" in args:
        print(_probe_send.__doc__, file=sys.stderr)
        return 0
    return _probe_send(args, is_call=True)


def cmd_observer(args: list[str]) -> int:
    """Launch the supervisor-GUI against the local cluster — ALWAYS over mTLS.

        theia observer [<machine>]

    The GUI binary falls back to an INSECURE channel when no CA is set; that is
    a production-leak hazard, so this verb REFUSES to start it without the dev
    mTLS material `theia manifest` stages under dist/manifest/<machine>/certs/.
    It exports THEIA_COM_TLS_CA/_CLIENT_CERT/_CLIENT_KEY (com REQUIRES the
    client cert when a CA is pinned) + THEIA_TRACE_DECODER_PATH (the pluggable
    pb decoders: the framework's runtime/services .so + the workspace's app .so)
    and points the GUI at the per-machine manifest dir. Build the GUI first
    (bazel build //tools/supervisor-gui:supervisor-gui) — we exec the staged
    binary, building it on demand if missing.

    No certs → hard error with the one-liner to stage them. Never insecure."""
    if "-h" in args or "--help" in args:
        print(cmd_observer.__doc__, file=sys.stderr)
        return 0

    certs = _client_certs_dir()
    need = ["ca.crt", "client.crt", "client.key"]
    if certs is None or any(not (certs / f).is_file() for f in need):
        # No staged mTLS material. The GUI must NEVER run insecure, so generate
        # the dev cert set on demand into dist/manifest/central/certs (the same
        # place `theia manifest` would stage it) rather than fall back to a
        # plaintext channel.
        # gen_dev_certs.sh is a FRAMEWORK tool (THEIA_ROOT/tools), not the
        # consuming workspace's; the certs stage into the WORKSPACE's manifest.
        gen = THEIA_ROOT / "tools" / "gen_dev_certs.sh"
        target = WORKSPACE / "dist" / "manifest" / "central" / "certs"
        if not gen.is_file():
            print("theia observer: no mTLS certs and no tools/gen_dev_certs.sh "
                  "— refusing to launch the GUI insecure.", file=sys.stderr)
            return 2
        print("theia observer: no mTLS certs staged — generating dev certs "
              f"→ {target}", file=sys.stderr)
        target.mkdir(parents=True, exist_ok=True)
        if (rc := _run(["bash", str(gen), str(target), "localhost"])) != 0:
            return rc
        certs = target
        missing = [f for f in need if not (certs / f).is_file()]
        if missing:
            print(f"theia observer: {certs} still missing {missing} after "
                  "gen_dev_certs.sh — refusing to launch insecure.",
                  file=sys.stderr)
            return 2

    # Build on demand, then resolve the staged binary (bazel nests it under a
    # per-target bin/ dir: bazel-bin/tools/supervisor-gui/supervisor-gui/bin/…).
    # The GUI is a FRAMEWORK tool (//tools/...), so it builds + lands under
    # THEIA_ROOT, not the consuming workspace.
    gui = (THEIA_ROOT / "bazel-bin" / "tools" / "supervisor-gui"
           / "supervisor-gui" / "bin" / "supervisor-gui")
    if not gui.is_file():
        print("theia observer: building the GUI "
              f"({_OBSERVER_TARGET})…", file=sys.stderr)
        if (rc := _run(["bazel", "build", _OBSERVER_TARGET],
                       cwd=THEIA_ROOT)) != 0:
            return rc
    if not gui.is_file():
        print(f"theia observer: GUI binary not found at {gui} after build.",
              file=sys.stderr)
        return 1

    manifest_dir = WORKSPACE / "dist" / "manifest"
    env = {
        **os.environ,
        "THEIA_COM_TLS_CA": str(certs / "ca.crt"),
        "THEIA_COM_TLS_CLIENT_CERT": str(certs / "client.crt"),
        "THEIA_COM_TLS_CLIENT_KEY": str(certs / "client.key"),
    }
    # per links libetcd-cpp-api.so; the GUI doesn't, but keep the loader path
    # consistent with `theia start` so a shared deps move doesn't bite.
    _lib = THEIA_ROOT / "third_party" / "etcd-cpp-apiv3" / "install" / "lib"
    if _lib.is_dir():
        prev = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = os.pathsep.join(
            [str(_lib)] + ([prev] if prev else []))

    gui_argv = [str(gui)]
    if manifest_dir.is_dir():
        gui_argv += ["--manifest-dir", str(manifest_dir)]
    print(f"theia observer: mTLS via {certs} → {_OBSERVER_TARGET}",
          file=sys.stderr)
    os.execve(str(gui), gui_argv, env)


def _deep_merge(base, over):
    """Recursively merge `over` INTO `base`, returning the merged value. Dicts
    merge key-by-key; a list of dicts each carrying a "name" merges BY NAME (so
    an executor.json `children` override patches a specific child, e.g. sets
    `run_on_start: false` on `per`, without rewriting the whole array); any
    other scalar/list in `over` REPLACES base. `over` wins on type conflict."""
    if isinstance(base, dict) and isinstance(over, dict):
        out = dict(base)
        for k, v in over.items():
            out[k] = _deep_merge(base[k], v) if k in base else v
        return out
    # Name-keyed list merge: both sides are lists whose elements are dicts with a
    # "name" — merge element-wise by name, keep base order, append new names.
    def _named(lst):
        return (isinstance(lst, list) and lst
                and all(isinstance(e, dict) and "name" in e for e in lst))
    if _named(base) and _named(over):
        out = []
        seen = set()
        for e in base:
            o = next((x for x in over if x["name"] == e["name"]), None)
            out.append(_deep_merge(e, o) if o else e)
            seen.add(e["name"])
        for x in over:                        # new children not in base
            if x["name"] not in seen:
                out.append(x)
        return out
    return over                               # scalar / plain list → replace


def _apply_config_overrides(machine: str, cfg_dir: Path) -> None:
    """Deep-merge deploy/config/<machine>/<name>.json ON TOP of the staged
    install/<machine>/config/<name>.json (INCLUDING executor.json). This is the
    LOCAL equivalent of colony's deploy-time config-override pass — it lets a
    per-machine override change any config without editing the .art, e.g.:

        deploy/config/central/executor.json
          {"children":[{"name":"services_sup","children":[
             {"name":"per","run_on_start":false},
             {"name":"nm", "run_on_start":false}]}]}

    blocks per (needs etcd) and nm (needs CAP_NET_ADMIN) from booting on a box
    that lacks those, while keeping them DEFINED in the tree. The build artifact
    stays a pure function of (arch, os, version); the override is the rig layer."""
    import json as _json
    override_dir = WORKSPACE / "deploy" / "config" / machine
    if not override_dir.is_dir():
        return
    for ov in sorted(override_dir.glob("*.json")):
        target = cfg_dir / ov.name
        if not target.is_file():
            continue                          # override for a config we didn't stage
        try:
            merged = _deep_merge(_json.loads(target.read_text()),
                                 _json.loads(ov.read_text()))
        except ValueError as e:
            print(f"theia: skipping malformed override {ov} ({e})",
                  file=sys.stderr)
            continue
        target.write_text(_json.dumps(merged, indent=2))
        print(f"theia: applied config override {ov.name} → {target}",
              file=sys.stderr)


def _installed_role_config(manifest_root: Path, machine: str) -> "Path | None":
    """The framework's shipped per-ROLE config slice for this machine, if the
    theia-services deb provided one — else None.

    The services deb stages /opt/theia/config/<role>/{<fc>.json,executor.json,
    config-defaults.json} (see `_inject_services_config`). This is the deb-mode
    fallback for `theia install` when the workspace didn't regenerate config
    (no dist/manifest/<machine>/config/). Resolve the machine → role via
    machines.json's role_map (a machine named after its role maps to itself),
    then return $THEIA_ROOT/config/<role> if it exists on disk."""
    import json as _json
    role = machine                                     # default: name IS the role
    try:
        mj = _json.loads((manifest_root / "machines.json").read_text())
        role = mj.get("role_map", {}).get(machine, machine)
    except (FileNotFoundError, KeyError, ValueError):
        pass
    cand = THEIA_ROOT / "config" / role
    if cand.is_dir():
        return cand
    # A single full-service box (the `central` bootstrap machine, or any host
    # with no explicit role) IS the `master` role — it runs the whole platform.
    # Fall back to the master slice so a bare deb-mode `theia install` still gets
    # its config. (A worker that named itself for the `zonal` role resolves above
    # via role_map; only the un-mapped/central case lands here.)
    master = THEIA_ROOT / "config" / "master"
    return master if master.is_dir() else None


def cmd_install(args: list[str]) -> int:
    """LOCAL install: build + populate $WORKSPACE/install/<machine>/.

        theia install <target> [--attr ATTR] [--machine M]

    The dev inner-loop counterpart of the remote .ipk deploy. <target> names a
    rig under manifest/<target>/rig.py (e.g. ``bootstrap``, or your workspace's
    own single / split / local targets) — the SAME target model as `theia
    manifest`. No default: a workspace's manifest/ targets vary, so <target>
    is mandatory. The deploy MANIFEST
    is the single source of truth: `artheia serialize-manifest manifest.<target>
    .rig --attr ATTR` first emits the AUTOSAR manifests, then the buildable
    binary set is READ BACK from the machine's execution.json (no hardcoded
    binary list). bazel builds those targets + artheia emits per-FC params;
    serialize-manifest already wrote executor.json (copied in); then _stage_local
    copies the binaries into install/<machine>/ and applies the SAME setcap
    contract a real (Ansible) deploy uses — "bazel builds, Python/Ansible stages
    the host". Puppet is gone.

    The machine to stage: --machine M, else the positional after <target>, else
    $THEIA_MACHINE, else the single machine in machines.json (a single-host
    target needs no name). The supervisor is the fixed framework runtime
    (//platform/supervisor/main:supervisor), built/staged separately.

    (`theia stage-local` is a back-compat alias for this verb.)"""
    # Target-based, mirroring cmd_manifest. The first positional is the TARGET;
    # the machine comes from --machine / $THEIA_MACHINE / machines.json (a 2nd
    # positional after the target is accepted as the machine for convenience).
    # No default — manifest/<target>/rig.py varies per workspace (theia init
    # scaffolds manifest/<name>/; nothing guarantees any particular target
    # name), so guessing wrong used to surface as a bare
    # ModuleNotFoundError deep in artheia's import machinery.
    positionals = [a for a in args if not a.startswith("-")]
    if not positionals:
        print("theia install: missing <target> — pass the manifest target "
              "(the manifest/<target>/rig.py dir name, e.g. "
              "`theia install bootstrap`).", file=sys.stderr)
        return 2
    target = positionals[0]
    attr = next((args[i + 1] for i, a in enumerate(args) if a == "--attr"), "RIG")
    machine = next((args[i + 1] for i, a in enumerate(args) if a == "--machine"),
                   None)
    if machine is None and len(positionals) > 1:
        machine = positionals[1]
    manifest_root = WORKSPACE / "install" / "manifest"
    module = f"manifest.{target}.rig"

    # 0. Address-collision gate — fail BEFORE building/staging if two nodes
    #    share a TIPC (type, instance) anywhere in the deployed FC set.
    if (rc := _check_tipc_addresses()) != 0:
        return rc

    # 1. The per-machine AUTOSAR manifest set (machine/application/service/
    #    execution/executor.json) → install/manifest/<machine>/. The source of
    #    truth for WHAT to build/stage — runs FIRST so the binary set is derived
    #    from it, not hand-listed. SAME command as `theia manifest`.
    if (rc := _run([
        "artheia", "serialize-manifest", module, "--attr", attr,
        "--rig-name", target, "--out", str(manifest_root),
    ])) != 0:
        return rc

    # 1a. Resolve the machine to install: --machine / 2nd positional (above),
    #     else $THEIA_MACHINE, else the SINGLE machine in machines.json (a
    #     single-host target needs no name). machines.json is a LIST OF NAMES.
    if machine is None:
        import json as _json
        machine = os.environ.get("THEIA_MACHINE")
        if machine is None:
            try:
                ms = _json.loads(
                    (manifest_root / "machines.json").read_text())["machines"]
            except (FileNotFoundError, KeyError):
                ms = []
            if len(ms) == 1:
                machine = ms[0]
            elif not ms:
                print("theia install: no machines in "
                      f"{manifest_root / 'machines.json'} — nothing to stage.",
                      file=sys.stderr)
                return 1
            else:
                print(f"theia install: multiple machines {ms} — pass one "
                      "(e.g. `theia install single --machine central`) or set "
                      "$THEIA_MACHINE.", file=sys.stderr)
                return 2
    dest = WORKSPACE / "install" / machine

    # 1b. Read the buildable binary set for THIS machine from execution.json:
    #     {bin-name: bazel target}, plus the fixed supervisor target (split out —
    #     it lands at <dest>/supervisor, not bin/).
    try:
        supervisor_target, binaries = _load_install_components(
            manifest_root, machine)
    except (FileNotFoundError, ValueError) as e:
        print(f"theia: {e}", file=sys.stderr)
        return 1

    # 1c. Host/admin machine guard: a machine with NO processes AND no supervisor
    #     tree (no executor.json) is a genuine host/admin node (console only) —
    #     stage just machines.json. But a BARE workspace (theia init, no services
    #     / apps yet) has an empty-but-present executor.json: it still runs the
    #     supervisor as a toolchain smoke test (tutorial ch2 §2.4 — "prove the
    #     toolchain works … even before you've written any app code"). So skip
    #     only when there's truly no supervisor tree to stage.
    _has_tree = (manifest_root / machine / "executor.json").is_file()
    if not binaries and not _has_tree:
        dest.mkdir(parents=True, exist_ok=True)
        src_machines = manifest_root / "machines.json"
        if src_machines.is_file():
            shutil.copy2(src_machines, dest / "machines.json")
        print(f"theia install: '{machine}' has no processes and no supervisor "
              f"tree (host/admin) — staged {dest / 'machines.json'} only.",
              file=sys.stderr)
        return 0

    # 2. bazel build — the supervisor + every child binary, partitioned by owner:
    #    FRAMEWORK targets (//platform/..., //services/...) build in THEIA_ROOT
    #    (the framework module owns them); the WORKSPACE's OWN app targets
    #    (//apps/..., everything else) build in WORKSPACE against @pero_theia (the
    #    workspace's MODULE.bazel). If the workspace has no MODULE.bazel, app
    #    targets fall back to THEIA_ROOT too (the framework's identical sources).
    #    In DEB mode the framework binaries (supervisor + ARA FCs) are PREBUILT
    #    under /opt/theia/bin — staged directly, never bazel-built — so only the
    #    workspace's OWN app C++ goes through bazel.
    all_targets = [supervisor_target, *binaries.values()]
    ws_has_module = (WORKSPACE / "MODULE.bazel").is_file()
    deb = _deb_mode()
    # name → prebuilt path for framework binaries we can stage as-is (deb mode).
    name_for = {supervisor_target: "supervisor", **{t: n for n, t in binaries.items()}}
    prebuilt = {}
    if deb:
        for t in all_targets:
            if _is_framework_target(t):
                pb = _prebuilt_bin(name_for[t])
                if pb is not None:
                    prebuilt[t] = pb
        if prebuilt:
            print(f"theia install: deb mode — staging {len(prebuilt)} prebuilt "
                  f"framework binaries from {THEIA_ROOT / 'bin'}", file=sys.stderr)
    fw_targets = [t for t in all_targets
                  if _is_framework_target(t) and t not in prebuilt]
    ws_targets = [t for t in all_targets if not _is_framework_target(t)]
    if fw_targets and (rc := _run(["bazel", "build", *fw_targets],
                                  cwd=THEIA_ROOT)) != 0:
        return rc
    if ws_targets:
        ws_build_root = WORKSPACE if ws_has_module else THEIA_ROOT
        if (rc := _run(["bazel", "build", *ws_targets],
                       cwd=ws_build_root)) != 0:
            return rc

    # 3. config/ — copy the entire config/ dir that serialize-manifest emitted
    #    (executor.json + config/<fc>.json + config-defaults.json). This is the
    #    authoritative source: PROCESS_PARAMS / PROCESS_CONFIG_DEFAULTS were
    #    captured at gen-manifest time and baked into the manifest module, so no
    #    .art backtrack is needed here. install/<machine>/config/ is a verbatim
    #    copy of dist/manifest/<machine>/config/ — byte-identical.
    dest.mkdir(parents=True, exist_ok=True)
    src_executor = manifest_root / machine / "executor.json"
    if not src_executor.is_file():
        print(f"theia install: no executor.json at {src_executor} — "
              "serialize-manifest did not emit the supervisor tree.",
              file=sys.stderr)
        return 1
    cfg_dir = dest / "config"
    cfg_dir.mkdir(parents=True, exist_ok=True)
    # Copy all JSON files from the manifest config/ dir (includes executor.json,
    # per-FC params, config-defaults). Fall back to copying executor.json alone
    # if the manifest was produced by an older serialize-manifest that didn't emit
    # a config/ dir (graceful degradation for partially-upgraded workspaces).
    src_cfg = manifest_root / machine / "config"
    if src_cfg.is_dir():
        for src_f in src_cfg.iterdir():
            if src_f.suffix == ".json":
                shutil.copy2(src_f, cfg_dir / src_f.name)
                print(f"staged {cfg_dir / src_f.name}", file=sys.stderr)
    elif binaries and (role_cfg := _installed_role_config(
            manifest_root, machine)) is not None:
        # Deb-mode fallback: a workspace WITH services (binaries non-empty) that
        # didn't regenerate config (no dist/manifest/<machine>/config/) — the
        # theia-services deb ships the per-ROLE defaults at /opt/theia/config/
        # <role>/. Copy that slice, selected by this machine's role, so a deb-only
        # install still gets its config (symmetric with source-tree / S3 paths).
        # Guarded on `binaries`: a BARE workspace (no FCs) must NOT pull the
        # master role config — it runs zero FCs, so only its empty executor.json.
        for src_f in role_cfg.glob("*.json"):
            shutil.copy2(src_f, cfg_dir / src_f.name)
            print(f"staged {cfg_dir / src_f.name} (from {role_cfg})", file=sys.stderr)
    else:
        # Older manifest layout — no config/ dir yet. Copy just executor.json.
        shutil.copy2(src_executor, cfg_dir / "executor.json")
        print(f"staged {cfg_dir / 'executor.json'} (legacy — re-run theia manifest)",
              file=sys.stderr)

    # 3b. Apply per-machine config overrides (deploy/config/<machine>/*.json),
    #     deep-merged onto the staged config — incl. executor.json (e.g.
    #     run_on_start:false to keep per/nm DOWN on a box without etcd/netadmin).
    _apply_config_overrides(machine, cfg_dir)

    # 4. Stage binaries + setcap. A binary's source is its prebuilt path (deb
    #    mode) when we have one, else its bazel-bin output.
    def _src(name: str, target: str) -> str:
        pb = prebuilt.get(target)
        return str(pb if pb is not None else _bazel_bin(target))
    bins = {n: _src(n, t) for n, t in binaries.items()}
    sup_src = _src("supervisor", supervisor_target)

    # Stage the binaries into install/<machine>/. In DEB mode the sources are the
    # prebuilt /opt/theia/bin/* (the supervisor already capped by the deb's
    # postinst), so SYMLINK them — no copy, no `sudo setcap` per install (the
    # user never needs sudo locally). In source mode they're bazel-out artifacts
    # that need a writable copy + setcap. `link` selects.
    link = _deb_mode() and all(
        str(p).startswith(str(THEIA_ROOT / "bin")) for p in [sup_src, *bins.values()])
    return _stage_local(dest, sup_src, bins, link=link)


_warned_legacy_machine_cfg: set = set()


def _warn_legacy_machine_config(machine: str) -> None:
    """Warn (once per machine) if a stale deploy/config/<machine>/ override dir
    exists — it is NO LONGER baked into the built artifact.

    Config overrides used to be deep-merged into the gen-params default here, at
    install/manifest time, keyed by the artheia `machine`. That coupled the
    build-once artifact to a rig's config and conflated machine (a manifest
    slice) with rig (a physical deploy target). Overrides are now a DEPLOY-time
    concern, applied per TARGET by colony's tasks/config-override.yml. A
    leftover deploy/config/<machine>/ dir is the legacy layout: tell the operator
    to re-key it under the deploy TARGET name (deploy/registry/<target>.yml) so
    it's applied at deploy time. Machine-shared config (e.g. central=GPS-
    grandmaster vs compute=slave) still works — colony also merges
    deploy/config/<machine>/ at deploy time using the target's resolved machine,
    just not baked into the artifact."""
    if machine in _warned_legacy_machine_cfg:
        return
    _warned_legacy_machine_cfg.add(machine)
    legacy = WORKSPACE / "deploy" / "config" / machine
    if legacy.is_dir() and any(legacy.glob("*.json")):
        files = ", ".join(sorted(p.name for p in legacy.glob("*.json")))
        print(
            f"WARNING: deploy/config/{machine}/ ({files}) is NOT baked into the "
            f"artifact — config overrides are applied at DEPLOY time, per rig.\n"
            f"         It still works if a deploy target runs the '{machine}' "
            f"machine (colony merges it at orchestrate time), but a per-RIG "
            f"change belongs under deploy/config/<target>/ "
            f"(deploy/registry/<target>.yml).",
            file=sys.stderr,
        )


def _emit_fc_config(machine: str, binaries: dict, cfg_dir: Path) -> int:
    """Emit the per-FC static params JSON (config/<fc>.json) for *machine* into
    *cfg_dir*, one file per FC that declares a params {} block.

    Read once at boot by the runtime config singleton (init_config(<fc>)
    resolves $THEIA_CONFIG_DIR/<fc>.json; the supervisor sets
    THEIA_CONFIG_DIR=config in the child env). A params-less FC emits an empty
    {nodes:{}} (harmless; lookups default).

    `binaries` maps fc name → its bazel target (so _fc_art_path resolves the
    .art). gen-params writes the machine-generic default; the hand-written
    per-machine override deploy/config/<machine>/<fc>.json (partial, same shape)
    is deep-merged on top so each machine gets its own profile (e.g. tsync
    central=GPS-grandmaster vs compute=PTP-slave) without forking the .art.

    SHARED by `theia install` (→ install/<machine>/config/) and `theia manifest`
    (→ dist/manifest/<machine>/config/) so a local stage and a remote deploy
    ship byte-identical per-FC config. Returns 0 on success, non-zero rc on a
    gen-params failure.

    The output is the MACHINE-GENERIC default ONLY — no deploy/config override is
    baked in here. Config overrides are a DEPLOY-time concern (per rig), applied
    by colony's tasks/config-override.yml against the running device, so the built
    artifact stays a pure function of (arch, os, version) — never rig-specific.
    See _warn_legacy_machine_config for the migration warning."""
    cfg_dir.mkdir(parents=True, exist_ok=True)
    _warn_legacy_machine_config(machine)
    for fc, target in binaries.items():
        art = _fc_art_path(fc, target)
        if art is None:
            continue
        out_json = cfg_dir / f"{fc}.json"
        if (rc := _run([
            "artheia", "gen-params", str(art), "--out", str(out_json),
        ])) != 0:
            return rc
    print(f"staged {cfg_dir}/<fc>.json (static params)")
    return 0


def _stage_certs(machines: list[dict], out: Path) -> int:
    """Stage the dev mTLS certs into each target machine's manifest dir.

    com↔GUI/rtdb run MUTUAL TLS when a CA is pinned. The dev cert set (CA +
    server + client, self-signed) lives in deploy/certs/, generated ONCE by
    tools/gen_dev_certs.sh and cached there (regenerating would invalidate any
    running client). We copy ca.crt + server.{crt,key} + client.{crt,key} into
    dist/manifest/<machine>/certs/, which the deploy bind-mounts at
    /etc/theia/manifest/<machine>/certs/; run-supervisor.sh exports THEIA_COM_TLS_*
    from there so the supervisor's com children serve mTLS, and rtdb/GUI point at
    the same CA + client identity.

    DEV certs only (self-signed, CN/SAN=localhost+127.0.0.1 — correct for the
    network_mode:host cluster where clients dial 127.0.0.1). A real deployment
    swaps in PKI-issued certs (or the crypto-FC engine slot, THEIA_COM_TLS_SLOT)."""
    certs_src = WORKSPACE / "deploy" / "certs"
    needed = ["ca.crt", "server.crt", "server.key", "client.crt", "client.key"]
    if not all((certs_src / f).is_file() for f in needed):
        gen = WORKSPACE / "tools" / "gen_dev_certs.sh"
        if not gen.is_file():
            print("theia manifest: no deploy/certs and no gen_dev_certs.sh — "
                  "skipping mTLS cert staging (cluster runs INSECURE).",
                  file=sys.stderr)
            return 0
        certs_src.mkdir(parents=True, exist_ok=True)
        if (rc := _run(["bash", str(gen), str(certs_src), "localhost"])) != 0:
            return rc
    for m in machines:
        if m.get("kind") == "host":
            continue
        dst = out / m["name"] / "certs"
        dst.mkdir(parents=True, exist_ok=True)
        for f in needed:
            shutil.copy2(certs_src / f, dst / f)
    print(f"theia manifest: staged dev mTLS certs → "
          f"dist/manifest/<machine>/certs/ (from deploy/certs/)", file=sys.stderr)
    return 0


def _emit_machine_config(machine: str, mdir: Path) -> int:
    """Emit per-FC static params into <mdir>/config/<fc>.json for one machine.

    The deploy counterpart of cmd_install's step 3b: for every buildable FC in
    THIS machine's application.json, run `gen-params <fc>.art` to write the
    machine-generic default, then deep-merge the per-machine override
    deploy/config/<machine>/<fc>.json (if present) on top. The result is what
    orchestration.pp copies into /opt/theia/config/ so the containerized FC
    boots with its REAL profile (e.g. tsync central=GPS-grandmaster vs
    compute=PTP-slave) — not just the .art slave default.

    The emitted config is the MACHINE-GENERIC default (gen-params) ONLY — no
    deploy/config override is baked in. A per-rig config change (e.g. the rpi4
    RTK-GPS box disabling ptp4l/gpsd) is a DEPLOY-time concern, applied by
    colony's tasks/config-override.yml against the device keyed by the deploy
    TARGET (the rig in deploy/registry/<target>.yml). Keeping it out of the
    artifact preserves build-once: dist/manifest/<machine>/ is a pure function of
    (arch, os, version), reused across every rig that runs this machine slice.
    mdir is the machine's manifest dir (dist/manifest/<machine>)."""
    import json as _json
    app = mdir / "application.json"
    if not app.is_file():
        return 0
    data = _json.loads(app.read_text())
    binaries: dict[str, str] = {}
    for a in data.get("applications", []):
        for c in a.get("components", []):
            if not c.get("bazel_buildable", True):
                continue
            name, target = c.get("name"), c.get("bazel_target")
            if name and target and name != "supervisor":
                binaries[name] = target
    cfg_dir = mdir / "config"
    cfg_dir.mkdir(parents=True, exist_ok=True)
    _warn_legacy_machine_config(machine)
    for fc, target in binaries.items():
        art = _fc_art_path(fc, target)
        if art is None:
            continue
        out_json = cfg_dir / f"{fc}.json"
        if (rc := _run([
            "artheia", "gen-params", str(art), "--out", str(out_json),
        ])) != 0:
            return rc
    return 0


def _stage_local(dest: Path, supervisor_src: str,
                 binaries: dict[str, str], link: bool = False) -> int:
    """Stage the supervisor + child binaries into install/<machine>/.

    The symlink-vs-copy decision is PER BINARY: a source already under
    $THEIA_ROOT/bin (a prebuilt, deb-installed binary) is SYMLINKED — no copy,
    and no setcap (the supervisor there is already capped by theia-runtime's
    postinst). A bazel-out source (the workspace's OWN app FCs, or a
    source-checkout framework build) is COPIED, and if the SUPERVISOR itself was
    copied it gets a `sudo setcap`.

    So the common deb case — prebuilt supervisor + workspace app bins — needs NO
    sudo: the supervisor is a capped symlink and the app bins are plain user-
    owned copies (FC children need no caps; the supervisor sets their thread
    scheduling). `link` (all-prebuilt) is kept as a fast-path hint but the
    per-binary check is authoritative.

    supervisor at <dest>/supervisor, children at <dest>/releases/local/bin/<name>.

    Supervisor caps:
      cap_sys_nice   — realtime sched (SCHED_FIFO/RR) + CPU affinity on FC
                       node threads.
      cap_net_admin  — TIPC admin ops the supervisor performs: `tipc node set
                       clusterid` (rig isolation) + the topology-service
                       presence subscription com relies on. (Plain TIPC name
                       bind does NOT need it; cluster/topology admin does.)"""
    import shutil as _sh
    dest.mkdir(parents=True, exist_ok=True)
    # Release-dir layout: supervisor binary at <dest>/supervisor (the updater,
    # never swapped); children at <dest>/releases/local/bin/, pointed at via the
    # <dest>/current symlink. theia start exports THEIA_INSTALL_DIR=<dest>/current,
    # so the supervisor resolves each child's "bin/<svc>" there. OTA flips
    # current→releases/<ver>; locally there's one release ("local").
    rel = dest / "releases" / "local"
    (rel / "bin").mkdir(parents=True, exist_ok=True)
    cur = dest / "current"
    # Atomically (re)point current → releases/local.
    if cur.is_symlink() or cur.exists():
        try:
            cur.unlink()
        except PermissionError:
            _run(["sudo", "rm", "-f", str(cur)])
    cur.symlink_to(Path("releases") / "local")

    # A legacy Puppet/sudo install left install/<machine>/ root-owned, so a
    # user-run stage can't overwrite files OR create new ones in bin/. If the
    # tree isn't writable by us, sudo-chown it back to the current user once.
    if not os.access(rel / "bin", os.W_OK):
        import getpass
        _run(["sudo", "chown", "-R", f"{getpass.getuser()}:{getpass.getuser()}",
              str(dest)])

    def _clear(dst: Path) -> None:
        if dst.exists() or dst.is_symlink():
            try:
                if not dst.is_symlink():
                    dst.chmod(0o755)
                dst.unlink()
            except PermissionError:
                _run(["sudo", "rm", "-f", str(dst)])

    _prebuilt_root = str(THEIA_ROOT / "bin")

    def _is_prebuilt(src: str) -> bool:
        # A source under $THEIA_ROOT/bin is a deb-installed binary → symlink it.
        return link or str(src).startswith(_prebuilt_root)

    def _place(src: str, dst: Path) -> None:
        _clear(dst)
        if _is_prebuilt(src):
            dst.symlink_to(src)               # → prebuilt /opt/theia/bin/* (capped)
            print(f"  linked {dst} → {src}", file=sys.stderr)
        else:
            _sh.copy2(src, dst)
            dst.chmod(0o755)
            print(f"  staged {dst}", file=sys.stderr)

    sup_copied = not _is_prebuilt(supervisor_src)
    try:
        _place(supervisor_src, dest / "supervisor")
        for name, src in binaries.items():
            _place(src, rel / "bin" / name)   # children → releases/local/bin (via current/)
    except OSError as e:
        print(f"theia install: staging failed — {e}", file=sys.stderr)
        return 1

    if not sup_copied:
        # The supervisor is a symlink to the already-capped /opt/theia/bin/
        # supervisor (theia-runtime postinst) — no copy cleared its caps, no
        # setcap needed, no sudo. App bins (if any were copied) are plain
        # user-owned FC executables that need no caps.
        print(f"theia install: staged {dest} (supervisor symlinked + capped by "
              "the deb; no sudo needed)", file=sys.stderr)
        return 0

    # The supervisor was COPIED (source build) — its caps were cleared, so setcap
    # it. Needs root; skip gracefully if setcap/sudo unavailable.
    setcap = shutil.which("setcap") or "/usr/sbin/setcap"
    sup = dest / "supervisor"
    rc = _run(["sudo", setcap, "cap_sys_nice,cap_net_admin+eip", str(sup)])
    if rc != 0:
        print("theia install: setcap cap_sys_nice,cap_net_admin failed (need "
              "root / libcap2-bin?) — supervisor runs without realtime priority "
              "/ TIPC cluster isolation.", file=sys.stderr)
    print(f"theia install: staged {dest}", file=sys.stderr)
    return 0


# The deploy split: `theia manifest` runs `artheia serialize-manifest
# manifest.<target>.rig` to emit the per-machine JSON, and `theia dist` then
# works purely from that committed JSON (RULE 2 — rules/dist_ipk.bzl, no rig.py
# eval at build time). Dev iteration uses `bazel build @rig_single//<host>:image`
# directly (RULE 1 — rules/rig.bzl, serialize-at-eval).
_MANIFEST_DIR = "dist/manifest"

# ── Cross-compile TARGET REGISTRY ────────────────────────────────────────────
# The Python mirror of rules/config/targets.bzl — the SINGLE place every target's
# (platform, bazel-config name, deb ABI key, deb arch, min-glibc) lives, so adding
# a board is ONE entry, not a rediscovery across theia.py + cmake + colony. A
# target is a (cpu, libc/distro) PAIR — rpi4 (bookworm/2.36) and jetson
# (focal/2.31) are DIFFERENT aarch64 ABIs with different sysroots + non-
# interchangeable binaries (the Jetson lesson: rpi4 binaries need GLIBC_2.34/2.38).
# Keep in lockstep with rules/config/targets.bzl::TARGETS.
_TARGETS = {
    "host":   {"cfg": "host",   "cpu": "x86_64",  "abi_key": "amd64",
               "deb_arch": "amd64", "libc_min": ""},
    "rpi4":   {"cfg": "rpi4",   "cpu": "aarch64", "abi_key": "bookworm-arm64",
               "deb_arch": "arm64", "libc_min": "2.36"},
    "jetson": {"cfg": "jetson", "cpu": "aarch64", "abi_key": "focal-arm64",
               "deb_arch": "arm64", "libc_min": "2.31"},
}


def _target(arch: str) -> dict | None:
    """arch token (a target NAME like rpi4/jetson/host, OR a bare cpu for the
    legacy default) → the target dict. Bare cpu picks the first matching target
    (aarch64→rpi4) for back-compat with machine.json arch=aarch64."""
    if arch in _TARGETS:
        return _TARGETS[arch]
    for t in _TARGETS.values():
        if t["cpu"] == arch:
            return t
    return None


def _platform_label(arch: str, qualified: bool = True) -> str | None:
    """The bazel --platforms label for an arch token. qualified=True uses the
    @pero_theia//… form (resolves from a consuming workspace too)."""
    t = _target(arch)
    if not t:
        return None
    prefix = "@pero_theia//rules/config:" if qualified else "//rules/config:"
    return prefix + t["cfg"]


# machine.json CPU arch → bazel platform label (the per-host cross-build). Derived
# from the registry; qualified @pero_theia//… so a consuming workspace resolves it.
_ARCH_PLATFORM = {a: _platform_label(a) for a in ("x86_64", "aarch64")}


def _emit_manifest_build_files(mdir: Path, machines: list[str]) -> None:
    """Drop the bazel glue alongside the serialized JSON so `theia dist` can
    reference each host's manifest files as labels and build a dist_pkg target
    (RULE 2 — rules/dist_ipk.bzl). Written by `theia manifest` because it owns
    (the gitignored) dist/manifest/.

    `machines` is the NAME LIST from machines.json (the serialize-manifest
    shape). Per-host BUILD exports the JSONs; the top-level BUILD declares one
    dist_pkg(name=<host>) per machine (the new shape has no host/target split —
    every serialized machine is a deploy target)."""
    for name in machines:
        (mdir / name / "BUILD.bazel").write_text(
            "# AUTO-GENERATED by `theia manifest`. exports the JSON manifests "
            "as bazel labels.\n"
            'package(default_visibility = ["//visibility:public"])\n'
            'exports_files(["machine.json", "application.json", '
            '"service.json", "execution.json", "executor.json"])\n'
        )
    import json as _json

    # Repo-qualify a process `executable` label the way dist_ipk.bzl's
    # ALL_BINARIES does: FRAMEWORK binaries live in @pero_theia, a consuming
    # workspace's OWN binaries in the ROOT module (@@). A bare `//services/...`
    # from a consuming workspace would resolve against @pero_theia, so qualify it.
    #
    # The framework owns ALL of //services/* but only the platform SUBDIRS it
    # actually ships: runtime/supervisor/proto/config. A consuming workspace may
    # carry its OWN //platform/<x> (e.g. gateway_ws's //platform/gateway — the
    # gateway lives in the private ws because it has PSP deps that can't be
    # exposed in the framework). Those are LOCAL (@@), NOT @pero_theia. So match
    # the real framework platform packages, not every `//platform/`.
    _FRAMEWORK_PLATFORM = ("//platform/runtime", "//platform/supervisor",
                           "//platform/proto", "//platform/config")
    def _qualify(label: str) -> str:
        if label.startswith("//services/") or \
                any(label.startswith(p) for p in _FRAMEWORK_PLATFORM):
            return "@pero_theia" + label
        return "@@" + label if label.startswith("//") else label

    lines = [
        "# AUTO-GENERATED by `theia manifest`. One .deb per host, packed from",
        "# that host's execution.json (rules/dist_ipk.bzl — RULE 2). The <host>_pkg",
        "# target emits .deb (default); pass format=\"ipk\" for the opkg hatch.",
        "# `binaries` is DERIVED from the host's execution.json (NOT the rule's",
        "# default ALL_BINARIES, which hardcodes the demo apps) so a service-only /",
        "# app-free manifest doesn't drag in app labels absent from this workspace.",
        "# Loaded via @pero_theia//rules so it resolves from a CONSUMING workspace",
        "# (no rules/ dir of its own) as well as in-framework.",
        'load("@pero_theia//rules:dist_ipk.bzl", "dist_pkg")',
        "",
    ]
    deb = _deb_mode()
    for h in machines:
        execu = _json.loads((mdir / h / "execution.json").read_text())
        labels = {
            _qualify(p["executable"])
            for p in execu.get("processes", [])
            if p.get("executable")
        }
        # EVERY machine runs the supervisor — it's the runtime that boots the
        # executor.json tree + is the PG allocator. It is NOT a process in
        # execution.json (it's the implicit root), so add it explicitly or the
        # .deb has the services but nothing to fork/supervise them.
        # In deb mode the supervisor is pre-installed (/opt/theia/bin/supervisor)
        # and is NOT a buildable target in the consuming workspace — omit it.
        # In source mode (THEIA_ROOT is the checkout) include it so `theia dist`
        # builds everything in one go.
        if not deb:
            labels.add(_qualify(_SUPERVISOR_TARGET))
        else:
            # DEB MODE: the framework binaries (@pero_theia//services/*,
            # //platform/*) are PREBUILT under /opt/theia/bin — the deb ships no
            # source for them, so they are NOT buildable targets here. Drop them
            # from `binaries` (dist stages them from the prefix, like `install`);
            # keep only the workspace's OWN binaries (@@//apps/…). A framework-
            # only manifest (the with-services bootstrap smoke test) then packs an
            # EMPTY buildable set — the .deb carries the app plane, the runtime
            # deb carries the services.
            labels = {l for l in labels if not l.startswith("@pero_theia")}
        bins = "".join(f'\n        "{lbl}",' for lbl in sorted(labels))
        # deb_mode tells pack_ipk to tolerate the prebuilt framework FCs missing
        # from the (app-only) filegroup — they come from the runtime deb.
        deb_attr = "\n    deb_mode = True," if deb else ""
        lines.append(
            f'dist_pkg(\n    name = "{h}",\n    binaries = [{bins}\n    ],{deb_attr}\n)')
    (mdir / "BUILD.bazel").write_text("\n".join(lines) + "\n")


def cmd_manifest(args: list[str]) -> int:
    """Serialize a TEST TARGET's rig to the per-machine JSON manifest set.

        theia manifest <target> [--attr ATTR] [--out DIR]

    <target> names a rig under manifest/<target>/rig.py (e.g. ``bootstrap``, or
    your workspace's own single / split / local targets). No default: a
    workspace's manifest/ targets vary, so <target> is mandatory. The rig
    assembles the services + apps manifests on the
    orthogonal engine and applies that target's deploy transform. This runs
    `artheia serialize-manifest manifest.<target>.rig --attr ATTR`, which
    validate()s the unmaterialized deployment FIRST (refusing on any
    inconsistency) then writes machines.json + per-machine
    {machine,service,application,execution,executor}.json under <out>
    (default dist/manifest/, i.e. install/manifest/ via theia install).

    --attr selects the rig's DeploymentLayer export (default ``RIG``; e.g.
    `theia manifest split --attr HW`)."""
    import json
    # Address-collision gate FIRST: a duplicate TIPC (type, instance) across FCs
    # silently mis-wires the runtime, so fail before emitting any manifest.
    if (rc := _check_tipc_addresses()) != 0:
        return rc

    # No default target — see cmd_install for why (no target name is
    # guaranteed to exist).
    target = next((a for a in args if not a.startswith("-")), None)
    if target is None:
        print("theia manifest: missing <target> — pass the manifest target "
              "(the manifest/<target>/rig.py dir name, e.g. "
              "`theia manifest bootstrap`).", file=sys.stderr)
        return 2
    attr = next((args[i + 1] for i, a in enumerate(args) if a == "--attr"), "RIG")
    out_arg = next((args[i + 1] for i, a in enumerate(args) if a == "--out"), None)
    out = Path(out_arg) if out_arg else WORKSPACE / _MANIFEST_DIR
    module = f"manifest.{target}.rig"

    # The rig NAME (single / split / …) = the manifest target dir name. Passed so
    # serialize-manifest writes it to machines.json `rig` and the user Software
    # Package is named from it (two rigs of one app → distinct SWPs, not both
    # "apps"). It's the robust source — survives the layer fold + arch/os rebuild
    # that drop a model-level MachineSetLayer.name.
    cmd = ["artheia", "serialize-manifest", module, "--attr", attr,
           "--rig-name", target, "--out", str(out)]
    if (rc := _run(cmd)) != 0:
        return rc

    machines = json.loads((out / "machines.json").read_text())["machines"]

    # SAFE BASE: deep-merge the per-machine deploy/config/<machine>/*.json
    # overrides (INCLUDING executor.json) onto the freshly-serialized manifest —
    # the SAME routine `theia install` uses (_apply_config_overrides). This bakes
    # rig-owned safe defaults (e.g. run_on_start:false for HW-gated FCs like
    # fw/tsync/rds) into the emitted manifest, so every consumer — the services
    # deb, the S3 manifest, and a user SWP that inherits this rig — deploys
    # cleanly on ANY rig regardless of HW/CAPA. An operator whose target HAS the
    # subsystem re-enables it with a per-machine override.
    for _m in machines:
        _apply_config_overrides(_m, out / _m / "config")
        # config/executor.json is what the supervisor reads; keep the top-level
        # machine executor.json (used by dist/deb staging) in sync.
        _cfg_ex = out / _m / "config" / "executor.json"
        _top_ex = out / _m / "executor.json"
        if _cfg_ex.is_file() and _top_ex.is_file():
            _top_ex.write_text(_cfg_ex.read_text())

    # config/<fc>.json and config/config-defaults.json are emitted by
    # serialize-manifest directly (from PROCESS_PARAMS / PROCESS_CONFIG_DEFAULTS
    # on the rig module — captured at gen-manifest time, no .art backtrack).
    # Log what landed so the operator can verify.
    for m in machines:
        cfg_dir = out / m / "config"
        if cfg_dir.is_dir():
            cfg_files = sorted(p.name for p in cfg_dir.iterdir()
                               if p.suffix == ".json")
            if cfg_files:
                print(f"  config/{m}: {', '.join(cfg_files)}", file=sys.stderr)

    # Drop the dist BUILD glue alongside the JSON so `theia dist` (RULE 2 —
    # rules/dist_ipk.bzl) can build //dist/manifest:<host>_pkg without re-running
    # the Python rig. Only when writing into the in-tree dist/manifest/ (the
    # default) — a custom --out is a throwaway serialize dir.
    if out == WORKSPACE / _MANIFEST_DIR:
        _emit_manifest_build_files(out, machines)
    print(f"theia manifest [{target}]: {len(machines)} machine(s) "
          f"{machines} → {out}/", file=sys.stderr)
    return 0


def cmd_dist(args: list[str]) -> int:
    """Build the per-machine .deb deploy artifacts from the serialized manifest.

        theia dist <target> [--arch A[,A…]]

    <target>  rig target name — same as `theia manifest <target>`. Mandatory:
              run `theia manifest <target>` first to emit dist/manifest/.
    --arch    OVERRIDE every machine's arch (comma-separated for several builds),
              e.g. `--arch host,rpi4`. The runtime plane ships per (arch, os), so
              one manifest → several arch builds. Omit → each machine's own arch
              from its machine.json.

    ONE build verb, keyed on whether the manifest is a RUNTIME or an APP manifest
    (the machines.json `apps` list — empty ⇒ runtime, non-empty ⇒ app):

      RUNTIME manifest (apps: [] — e.g. `theia dist services`): the manifest is
        ASSOCIATED with the framework runtime Deb set. Builds the versioned
        theia-runtime + theia-services .debs (packaging/theia) per arch — the
        artifacts colony installs on a fresh board (mirrors the old
        `theia release --arch`, which was a build-only misnomer).

      APP manifest (apps: [...] — e.g. `theia dist single`): NO Deb-set
        association, so pack EVERYTHING for the machine into ONE .deb per machine
        (//dist/manifest:<host>_pkg, rules/dist_ipk.bzl) — the self-contained app
        bundle Ansible copies + dpkg -i's. Staged next to the machine's manifest.

    `theia release <target>` builds via this verb then pushes to S3."""
    import json
    if "-h" in args or "--help" in args:
        print(cmd_dist.__doc__, file=sys.stderr)
        return 0
    # The bare positional is the target — but SKIP option VALUES (`--arch host`'s
    # "host" is not a target). Track tokens consumed by a value-taking option.
    _VALUE_OPTS = {"--arch", "--attr", "--distro", "--version"}
    _skip = {i + 1 for i, a in enumerate(args)
             if a in _VALUE_OPTS and i + 1 < len(args)}
    target = next((a for i, a in enumerate(args)
                   if not a.startswith("-") and i not in _skip), None)
    if not target:
        print("theia dist: <target> is required — e.g. `theia dist services` "
              "(runtime) or `theia dist single` (app).", file=sys.stderr)
        return 1
    # --arch overrides every machine's arch (release the runtime for another
    # arch/os from the same manifest). None → per-machine arch from machine.json.
    arch_override = None
    for i, a in enumerate(args):
        if a == "--arch" and i + 1 < len(args):
            arch_override = [x.strip() for x in args[i + 1].split(",") if x.strip()]
    machines_json = WORKSPACE / _MANIFEST_DIR / "machines.json"
    if not machines_json.is_file():
        print(f"theia dist: no manifest at {machines_json} — run `theia "
              f"manifest {target}` first.", file=sys.stderr)
        return 1
    mdir = machines_json.parent
    mdoc = json.loads(machines_json.read_text())
    # machines.json is a NAME LIST ({"machines":["central",...]}); every machine
    # is a deploy target. `apps` empty ⇒ this is the RUNTIME manifest (associated
    # with the theia-runtime/theia-services Deb set); non-empty ⇒ an APP manifest
    # (pack one self-contained .deb per machine).
    machines = mdoc["machines"]
    is_runtime = not mdoc.get("apps")

    if is_runtime:
        return _dist_runtime(mdir, machines, arch_override)
    return _dist_app(mdir, machines, arch_override)


def _dist_app(mdir: Path, machines: list, arch_override) -> int:
    """APP manifest → ONE self-contained .deb per machine (the Ansible bundle).

    //dist/manifest:<host>_pkg (rules/dist_ipk.bzl), cross-compiled per machine.
    Staged next to the machine's manifest JSON where orchestrate.yml reads it."""
    import json
    import shutil
    rc_final = 0
    for host in machines:
        mj = mdir / host / "machine.json"
        if not mj.is_file():
            print(f"theia dist: missing {mj}", file=sys.stderr)
            return 1
        # --arch overrides the machine's own arch (one build here — the app bundle
        # deploys to a known board, so a single override arch applies to all).
        arch = (arch_override[0] if arch_override
                else json.loads(mj.read_text())["arch"])
        platform = _ARCH_PLATFORM.get(arch)
        if not platform:
            print(f"theia dist: {host}: no bazel platform for arch '{arch}'",
                  file=sys.stderr)
            return 1
        if (rc := _run([
            "bazel", "build",
            f"//{_MANIFEST_DIR}:{host}_pkg",
            f"--platforms={platform}",
        ])) != 0:
            rc_final = rc
            continue
        # Stage the built .deb NEXT TO that host's manifest JSON, where the
        # deploy tooling reads it: orchestrate.yml/install-bundle.yml copy
        # `{{ manifest_dir }}/{{ machine }}/{{ machine }}.deb`. bazel leaves it
        # under bazel-bin/ as a SYMLINK into the output tree; Ansible's copy
        # (and scp) follow a regular file, not a dangling symlink across hosts,
        # so we copy the resolved bytes. This closes the dist→orchestrate chain
        # so a deploy needs NO hand-copy of the artifact.
        built = WORKSPACE / "bazel-bin" / _MANIFEST_DIR / f"{host}.deb"
        if not built.is_file():
            print(f"theia dist: {host}: built target ok but {built} missing "
                  "— cannot stage for deploy", file=sys.stderr)
            rc_final = 1
            continue
        staged = mdir / host / f"{host}.deb"
        staged.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(built, staged)   # resolves the bazel symlink → real file
        print(f"theia dist: staged {staged} ({staged.stat().st_size} bytes)")
    return rc_final


# The two runtime service ROLES the services deb carries config for. `master` is
# the full-platform coordinator (all FCs); `zonal` is the per-board worker slice
# (ucm + shwa). The rig declares both as machines (name == role), so one
# serialize pass emits both slices — see _generate_role_configs.
_SERVICES_ROLES = ("master", "zonal")


def _generate_role_configs(dest_root: Path) -> int:
    """Serialize the framework services rig and lay each ROLE's per-FC config
    JSON (+ executor.json + config-defaults.json) under dest_root/<role>/. This
    is the SAME config `theia manifest` emits per machine — keyed by ROLE so the
    theia-services deb can ship it at /opt/theia/config/<role>/ and `theia
    install` selects by the machine's role (closing the deb-path gap vs the
    local-install / S3-manifest paths, which already carry config). Returns 0 on
    success.

    The rig declares exactly two machines whose NAME is their ROLE (master,
    zonal), so one serialize pass yields both role slices — copy each machine's
    config/ dir straight to dest_root/<role>/.
    """
    import shutil
    import tempfile
    # serialize-manifest must import manifest.services.rig — resolvable from
    # THEIA_ROOT (the framework's manifest/ package lives there).
    env = {**os.environ, "PYTHONPATH": f"{THEIA_ROOT}{os.pathsep}"
           + os.environ.get("PYTHONPATH", "")}
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "rig"
        argv = ["artheia", "serialize-manifest", "manifest.services.rig",
                "--arch", "x86_64", "--out", str(out)]
        if _run(argv, cwd=THEIA_ROOT) != 0:
            # Fall back to the env-injected import path if cwd alone didn't put
            # manifest.* on sys.path.
            proc = subprocess.run(argv, cwd=THEIA_ROOT, env=env)
            if proc.returncode != 0:
                print("theia release: role config gen failed", file=sys.stderr)
                return proc.returncode
        # Machine name == role (master, zonal): copy each machine's config/.
        for role in ("master", "zonal"):
            mcfg = out / role / "config"
            if not mcfg.is_dir():
                print(f"theia release: no config/ for role {role}",
                      file=sys.stderr)
                return 1
            dst = dest_root / role
            dst.mkdir(parents=True, exist_ok=True)
            files = list(mcfg.glob("*.json"))
            for f in files:
                shutil.copy2(f, dst / f.name)
            print(f"theia release: role config {role} → {dst} "
                  f"({len(files)} files)")
    return 0


def _inject_services_config(services_deb: Path) -> int:
    """Repack a built theia-services .deb with per-role config under
    /opt/theia/config/<role>/. The bazel pkg_deb stays config-free (it can't run
    serialize-manifest); `theia release` injects the generated role slices here
    so the on-device services deb carries its own defaults — symmetric with the
    local `theia install` and S3-manifest paths. Root-owns the added files."""
    import shutil
    import tempfile
    if not services_deb.is_file():
        return 0
    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        root = tdp / "root"
        ctrl = tdp / "ctrl"
        root.mkdir(); ctrl.mkdir()
        # Unpack data + control.
        if (rc := _run(["dpkg-deb", "-R", str(services_deb), str(root)])) != 0:
            return rc
        cfg_root = root / "opt" / "theia" / "config"
        if (rc := _generate_role_configs(cfg_root)) != 0:
            return rc
        # Repack root-owned (dpkg-deb -b honors --root-owner-group).
        rebuilt = tdp / services_deb.name
        if (rc := _run(["dpkg-deb", "--build", "--root-owner-group",
                        str(root), str(rebuilt)])) != 0:
            return rc
        # The staged deb was copied from a read-only bazel output (copy2 preserves
        # mode), so make it writable before overwriting in place.
        services_deb.chmod(0o644)
        shutil.copyfile(rebuilt, services_deb)
        print(f"theia release: injected /opt/theia/config/{{{','.join(_SERVICES_ROLES)}}} "
              f"into {services_deb.name}")
    return 0


def _dist_runtime(mdir: Path, machines: list, arch_override) -> int:
    """RUNTIME manifest → the framework theia-runtime + theia-services Deb set.

    The manifest is ASSOCIATED with the runtime plane's prebuilt package targets
    (packaging/theia:theia-{runtime,services}_deb) rather than a per-machine
    pack — colony installs these on a fresh board. Builds per DISTINCT arch (each
    machine's own arch, or the --arch override list), collects the .debs into
    dist/debian/<pkg>/ (the same layout `theia release` reads to push to S3)."""
    import json
    import shutil
    # The set of arches to build. --arch overrides; else the union of the
    # machines' own arches (a mixed-arch runtime manifest builds each once).
    if arch_override:
        arches = arch_override
    else:
        arches = sorted({json.loads((mdir / h / "machine.json").read_text())["arch"]
                         for h in machines})
    deb_dir = WORKSPACE / _DIST_DEBIAN
    deb_dir.mkdir(parents=True, exist_ok=True)
    # The two on-device runtime debs (supervisor + services) — the association.
    runtime_targets = ["//packaging/theia:theia-runtime_deb",
                       "//packaging/theia:theia-services_deb"]
    rc_final = 0
    for arch in arches:
        # --arch takes registry tokens (host/rpi4/jetson); a machine.json arch is
        # a bazel arch (x86_64/aarch64). Accept either → a bazel platform label.
        platform = _RELEASE_ARCH.get(arch) or _ARCH_PLATFORM.get(arch)
        if not platform:
            print(f"theia dist: no bazel platform for arch '{arch}' "
                  f"(known: {', '.join(sorted(set(_RELEASE_ARCH) | set(_ARCH_PLATFORM)))})",
                  file=sys.stderr)
            return 1
        if (rc := _run(["bazel", "build", *runtime_targets,
                        f"--platforms={platform}"])) != 0:
            rc_final = rc
            continue
        # Collect the built .debs into dist/debian/<pkg>/ (bazel outputs are
        # read-only; unlink any stale dest before copy2). `theia release` reads
        # this same layout to push the runtime plane to S3.
        bin_root = WORKSPACE / "bazel-bin" / "packaging" / "theia"
        for f in bin_root.glob("*.deb"):
            pkg = f.name.split("_")[0]      # theia-runtime_0.2.2_amd64.deb → theia-runtime
            if pkg not in ("theia-runtime", "theia-services"):
                continue
            dst = deb_dir / pkg
            dst.mkdir(parents=True, exist_ok=True)
            dst_f = dst / f.name
            if dst_f.exists():
                dst_f.unlink()
            shutil.copy2(f, dst_f)
            print(f"theia dist: {arch}: staged {dst_f.name} → {dst}/")
            # The on-device services deb carries its own per-role config defaults
            # at /opt/theia/config/<role>/ — inject them here (the bazel pkg_deb
            # is config-free; serialize-manifest runs at release time). Arch-
            # independent (config JSON is params, not binaries), so injecting per
            # built services deb is fine.
            if pkg == "theia-services":
                if (rc := _inject_services_config(dst_f)) != 0:
                    rc_final = rc
    return rc_final



# ── theia release — build the installable package set (.deb + .ipk) ──────────
_DIST_DEBIAN = "dist/debian"
_DIST_IPKG = "dist/ipkg"

# arch token (from --arch) → bazel platform label, derived from the target
# registry. host = native amd64; rpi4/jetson = aarch64 cross-builds (each needs its
# sysroot + the cross toolchain). Adding a board = a _TARGETS entry, nothing here.
_RELEASE_ARCH = {a: _platform_label(a, qualified=False) for a in _TARGETS}

# The bazel-buildable package targets: (deb_target, ipk_target). Python wheels
# (framework, rf) + the CMake GUI are handled out-of-band below. The .ipk is the
# opt-in hatch (theia release --ipk); .deb is the default. Runtime/services split
# into machine + -dev: the .ipk (embedded) is machine-only — a -dev package is
# build-time, never shipped to an embedded target, so it has no .ipk.
_RELEASE_BAZEL_PKGS = [
    ("//packaging/theia:theia-runtime_deb",      "//packaging/theia:theia-runtime_ipk"),
    ("//packaging/theia:theia-runtime-dev_deb",  None),
    ("//packaging/theia:theia-services_deb",     "//packaging/theia:theia-services_ipk"),
    ("//packaging/theia:theia-services-dev_deb",  None),
    ("//packaging/theia:theia-system-dev_deb",   None),
]


def _build_framework_deb(out_dir: Path, version: str = "0.3.0") -> int:
    """Build theia-framework as a real .deb — WHEELS-AS-DATA, no system Python.

    Theia does NOT own the user's Python. The deb ships only:
      wheels/*.whl    artheia + rf-theia + their deps, as WHEELS. The USER drops
                      them into THEIR OWN venv:
                        python3 -m venv .venv && . .venv/bin/activate
                        pip install --find-links /opt/theia/wheels artheia rf-theia
                      (or from a pip server). No postinst, no /opt/theia/lib/python,
                      no writes to system site-packages.
      rules/, toolchains/   the bazel rules a workspace's MODULE.bazel needs.
      bin/{theia,artheia,artheia-lsp,artheia-mcp}   shims — `theia` is pure
                      stdlib (works before any venv, for init/manifest/install);
                      the artheia ones exec the artheia from the user's ACTIVE
                      venv (PATH), erroring with the pip line if absent.
      setup.{bash,zsh}      `source` to put /opt/theia/bin on PATH + THEIA_ROOT.

    Architecture: all (pure data + shell shims). Built with dpkg-deb."""
    import shutil

    pkg_root = WORKSPACE / "packaging" / "theia" / "framework"
    stage = out_dir / "_stage"
    if stage.exists():
        shutil.rmtree(stage)
    opt = stage / "opt" / "theia"
    (opt / "bin").mkdir(parents=True, exist_ok=True)
    wheels = opt / "wheels"
    wheels.mkdir(parents=True, exist_ok=True)

    # artheia + rf-theia WHEELS (the user pip-installs these into their venv).
    for src in (WORKSPACE / "artheia", WORKSPACE / "rf-theia"):
        if (src / "pyproject.toml").is_file():
            if (rc := _run([sys.executable, "-m", "pip", "wheel", "--no-deps",
                            "-w", str(wheels), str(src)])) != 0:
                return rc
    # Their third-party deps as wheels too, so the user's `pip install
    # --find-links /opt/theia/wheels artheia rf-theia` resolves fully OFFLINE.
    # Derive the dep set from the packages' OWN metadata (pip download <src>/)
    # rather than hand-listing — picks up artheia's (textX/Jinja2/click/PyYAML/
    # fastmcp) AND rf-theia's (robotframework/grpcio/numpy/pandas/asteval/…).
    # nanopb is added explicitly (its generator CLI is used at build time).
    dep_srcs = [str(WORKSPACE / d) for d in ("artheia", "rf-theia")
                if (WORKSPACE / d / "pyproject.toml").is_file()]
    # nanopb: proto genrule CLI. watchdog/pathspec: the work-with-me skill server
    # (shipped under /opt/theia/skills, wired by the scaffolded .mcp.json) — its
    # deps beyond mcp/fastmcp, bundled so the workspace venv resolves them offline.
    if (rc := _run([sys.executable, "-m", "pip", "download",
                    "--dest", str(wheels), "nanopb>=0.4.9",
                    "watchdog>=4.0.0", "pathspec>=0.12.0", *dep_srcs])) != 0:
        print("theia release: dep wheel download failed — the user will need "
              "PyPI reachable when they pip-install artheia/rf-theia.",
              file=sys.stderr)

    # bazel rules + toolchains the downstream workspace's MODULE.bazel needs.
    for d in ("rules", "toolchains"):
        if (WORKSPACE / d).is_dir():
            shutil.copytree(WORKSPACE / d, opt / d, dirs_exist_ok=True)
    # Make /opt/theia the `pero_theia` BAZEL MODULE root (same name+version as the
    # repo). A consuming ws does bazel_dep(pero_theia)+local_path_override(/opt/
    # theia), and @pero_theia//platform/runtime|supervisor/tombstone resolve
    # against the tree theia-runtime-dev ships at /opt/theia/platform/... The root
    # BUILD makes the module dir a valid package.
    shutil.copy2(WORKSPACE / "packaging" / "theia" / "module" / "MODULE.bazel",
                 opt / "MODULE.bazel")
    if not (opt / "BUILD.bazel").exists():
        (opt / "BUILD.bazel").write_text(
            '# pero_theia module root (the installed /opt/theia).\n'
            'package(default_visibility = ["//visibility:public"])\n')
    # theia.py itself (the workspace lifecycle driver) + setup scripts. It lives
    # under tools/ in the source tree; mirror that into the deb so the in-root
    # `theia` shim and the deb bin shim resolve it the same way.
    (opt / "tools").mkdir(parents=True, exist_ok=True)
    shutil.copy2(THEIA_ROOT / "tools" / "theia.py", opt / "tools" / "theia.py")
    for s in ("setup.sh",):
        shutil.copy2(pkg_root / s, opt / s)
    # tdb / rtdb — the Python live-inspect CLIs. They're NOT bazel artifacts and
    # NOT in the artheia wheel: their code lives under tools/{tdb,rtdb}/. Ship it
    # so `tdb`/`rtdb` work from a deb install (tutorial ch2 §2.4). tdb.py derives
    # REPO = parents[2] of its own path → /opt/theia (has system/tools/tdb/tdb.art
    # + platform/proto), and imports artheia.probe from the user's venv (on PATH).
    for probe_cli in ("tdb", "rtdb"):
        src = THEIA_ROOT / "tools" / probe_cli
        if not src.is_dir():
            continue
        shutil.copytree(
            src, opt / "tools" / probe_cli, dirs_exist_ok=True,
            ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    # theia MCP server — tools/theia_mcp.py (the dev-loop + tdb/rtdb + colony
    # tools as MCP). Ship it so a consuming workspace's scaffolded .mcp.json can
    # start the `theia` server against the installed prefix: the workspace venv's
    # python runs /opt/theia/tools/theia_mcp.py with PYTHONPATH=/opt/theia/tools
    # (so `import theia` resolves) and THEIA_INVOCATION_CWD=<workspace>.
    if (THEIA_ROOT / "tools" / "theia_mcp.py").is_file():
        shutil.copy2(THEIA_ROOT / "tools" / "theia_mcp.py",
                     opt / "tools" / "theia_mcp.py")

    # Skills — the agent skills (theia orientation, tasks, work-with-me) → /opt/
    # theia/skills. A consuming workspace points its tooling here (and the
    # scaffolded .mcp.json launches work-with-me's server from this tree). Drop
    # heavy/dev-only bits (venvs, caches, node_modules) from the copy.
    skills_src = THEIA_ROOT / "contrib" / "skills"
    if skills_src.is_dir():
        shutil.copytree(
            skills_src, opt / "skills", dirs_exist_ok=True,
            ignore=shutil.ignore_patterns(
                "__pycache__", "*.pyc", ".venv", "node_modules", "*.egg-info"))

    # `theia` launcher — PURE STDLIB (theia.py imports nothing from artheia), so
    # `theia init/manifest/install/start` work before the user has a venv.
    (opt / "bin" / "theia").write_text(
        '#!/bin/sh\nexec python3 "$(dirname "$0")/../tools/theia.py" "$@"\n')
    (opt / "bin" / "theia").chmod(0o755)
    # artheia / -lsp / -mcp shims — exec the artheia from the user's ACTIVE venv
    # (resolved on PATH, skipping THIS shim dir to avoid recursion). The Python
    # layer lives in the user's venv, NOT under /opt/theia. If artheia isn't
    # importable, print the one-line pip install and exit non-zero.
    _PIP_HINT = ("artheia is not installed in your active Python. Create/activate "
                 "a venv and install it:\\n"
                 "  python3 -m venv .venv && . .venv/bin/activate\\n"
                 "  pip install --find-links /opt/theia/wheels artheia rf-theia")
    for name, mod in (("artheia", "artheia.cli"),
                      ("artheia-lsp", "artheia.lsp.server"),
                      ("artheia-mcp", "artheia.adapters.mcp_server")):
        shim = opt / "bin" / name
        # Find <name> on PATH excluding our own bin dir, else fall back to the
        # current python -m if artheia imports, else print the hint.
        shim.write_text(
            "#!/bin/sh\n"
            'D="$(cd "$(dirname "$0")" && pwd)"\n'
            f'real=$(PATH=$(echo "$PATH" | tr ":" "\\n" | grep -v "^$D$" '
            f'| paste -sd:) command -v {name} 2>/dev/null)\n'
            f'[ -n "$real" ] && exec "$real" "$@"\n'
            f'python3 -c "import {mod}" 2>/dev/null && '
            f'exec python3 -m {mod} "$@"\n'
            f'printf "%b\\n" "{_PIP_HINT}" >&2\nexit 127\n')
        shim.chmod(0o755)

    # tdb / rtdb shims — run the shipped tools/{tdb,rtdb}/<cli>.py directly. They
    # import artheia.probe from the user's ACTIVE venv (on PATH), so guard on
    # artheia being importable and print the pip hint otherwise, same as above.
    for cli in ("tdb", "rtdb"):
        if not (opt / "tools" / cli / f"{cli}.py").is_file():
            continue
        shim = opt / "bin" / cli
        shim.write_text(
            "#!/bin/sh\n"
            'D="$(cd "$(dirname "$0")" && pwd)"\n'
            # Guard on the ACTUAL submodule the CLI needs (a bare `import artheia`
            # can succeed on an empty namespace-package stub whose __file__ is
            # None, then crash deeper) — check artheia.gen_server.probe.
            'python3 -c "import artheia.gen_server.probe" 2>/dev/null && '
            f'exec python3 "$D/../tools/{cli}/{cli}.py" "$@"\n'
            f'printf "%b\\n" "{_PIP_HINT}" >&2\nexit 127\n')
        shim.chmod(0o755)

    # control — pure data + shims; depends only on python3 (no pip, no postinst).
    ctrl = stage / "DEBIAN"
    ctrl.mkdir(parents=True, exist_ok=True)
    installed_kb = int(_du_kb(opt))
    (ctrl / "control").write_text(
        "Package: theia-framework\n"
        f"Version: {version}\n"
        "Architecture: all\n"
        "Section: devel\n"
        "Priority: optional\n"
        "Maintainer: Theia <theia@example.com>\n"
        f"Installed-Size: {installed_kb}\n"
        "Depends: python3\n"
        "Description: Theia framework — artheia/rf-theia wheels + bazel rules + CLIs\n")

    deb = out_dir / f"theia-framework_{version}_all.deb"
    if (rc := _run(["dpkg-deb", "--build", "--root-owner-group",
                    str(stage), str(deb)])) != 0:
        return rc
    shutil.rmtree(stage, ignore_errors=True)
    print(f"theia release: framework .deb → {deb}", file=sys.stderr)
    return 0


def _du_kb(path: Path) -> int:
    out = subprocess.run(["du", "-sk", str(path)], capture_output=True, text=True)
    try:
        return int(out.stdout.split()[0])
    except (ValueError, IndexError):
        return 0


def _release_runtime_plane(target: str, args: list[str]) -> int:
    """Build (via `theia dist <target>`) + push the RUNTIME plane to S3.

    The runtime-plane counterpart to `theia release-swp` (the app/SWP plane):
      theia release <target> [--arch A] [--version V] [--s3 URL] [--bucket B]

    Builds the theia-runtime + theia-services .debs from the manifest (theia dist
    associates a runtime manifest with that Deb set), then `aws s3 cp`s them to
    s3://<bucket>/<ver>-<abi>/ — the versioned runtime plane colony provisions a
    fresh board from (the `runtime_build` a Distribution role references). Omit
    --s3 to build + stage locally only (dist/debian/)."""
    import json
    import os
    import shutil
    import subprocess

    def _opt(name, default=None):
        for i, a in enumerate(args):
            if a == name and i + 1 < len(args):
                return args[i + 1]
        return default

    ver = _opt("--version", "0.3.0")
    s3_url = _opt("--s3")
    bucket = _opt("--bucket") or os.environ.get("THEIA_RUNTIME_BUCKET", "theia-runtime")
    arch_opt = _opt("--arch")

    # Build the runtime debs into dist/debian/ via the shared dist path (passes
    # --arch through so the runtime is released for the requested arch/os).
    dist_args = [target] + (["--arch", arch_opt] if arch_opt else [])
    if (rc := cmd_dist(dist_args)) != 0:
        print("theia release: dist build failed — nothing to push.",
              file=sys.stderr)
        return rc

    deb_dir = WORKSPACE / _DIST_DEBIAN
    debs = sorted(deb_dir.glob("theia-runtime/*.deb")) \
        + sorted(deb_dir.glob("theia-services/*.deb"))
    if not debs:
        print(f"theia release: no runtime debs under {deb_dir} after dist — is "
              f"'{target}' a runtime manifest (machines.json apps: [])?",
              file=sys.stderr)
        return 1

    if not s3_url:
        print(f"theia release: built {len(debs)} runtime .deb(s) → {deb_dir}/ "
              "(no --s3 → local only).", file=sys.stderr)
        for d in debs:
            print(f"  {d.relative_to(WORKSPACE)}", file=sys.stderr)
        return 0

    if not shutil.which("aws"):
        print("theia release: aws cli not found — built the debs; push from a "
              "host that has it.", file=sys.stderr)
        return 1
    env = {**os.environ,
           "AWS_ACCESS_KEY_ID": os.environ.get("MINIO_USER", "theia"),
           "AWS_SECRET_ACCESS_KEY": os.environ.get("MINIO_PASSWORD", "theiaminio"),
           "AWS_DEFAULT_REGION": "us-east-1"}
    aws = ["aws", "--endpoint-url", s3_url, "s3"]
    subprocess.run([*aws, "mb", f"s3://{bucket}"], env=env)  # idempotent
    rc_final = 0
    pushed = []
    # Group debs by KEY (<ver>-<abi>) — a mixed-arch runtime manifest lands each
    # board's slice under its own key. abi from the deb's arch suffix.
    import hashlib
    by_key: dict = {}
    for d in debs:
        abi = d.stem.rsplit("_", 1)[-1]      # theia-runtime_0.2.2_amd64 → amd64
        by_key.setdefault(f"{ver}-{abi}", []).append((d, abi))
    for key, entries in by_key.items():
        deb_meta = []
        for d, abi in entries:
            dst = f"s3://{bucket}/{key}/{d.name}"
            print(f"$ aws cp {d.name} {dst}", file=sys.stderr)
            if subprocess.run([*aws, "cp", str(d), dst], env=env).returncode != 0:
                rc_final = 1
                continue
            pushed.append(f"{key}/{d.name}")
            deb_meta.append({"file": f"{key}/{d.name}",
                             "sha256": hashlib.sha256(d.read_bytes()).hexdigest()})
        # The runtime-plane index the GS catalog reads (GET /api/planes/runtime →
        # <key>/index.json). WITHOUT it the debs upload but the GS never lists the
        # runtime — a Distribution can't reference it. Schema mirrors release-swp's.
        _ver, _abi = key.rsplit("-", 1)
        idx = {"plane": "runtime", "version": _ver, "distro": _abi,
               "key": key, "debs": deb_meta}
        idx_path = deb_dir / f"index-{key}.json"
        idx_path.write_text(json.dumps(idx, indent=2))
        if subprocess.run([*aws, "cp", str(idx_path),
                           f"s3://{bucket}/{key}/index.json"], env=env).returncode != 0:
            rc_final = 1
        else:
            print(f"$ aws cp index.json s3://{bucket}/{key}/index.json",
                  file=sys.stderr)
        # ── The per-machine MANIFEST + CONFIG (executor.json + config/<fc>.json).
        # colony no longer reads these from a local $THEIA_WORKSPACE — it pulls
        # <key>/manifest/<machine>/ from S3 (fetch-manifest-s3.yml). Ship the whole
        # serialized tree under the runtime key so a fresh board is self-serving
        # from S3: `theia manifest <target>` wrote it to dist/manifest/ (cmd_dist
        # ran it), so sync that dir. `aws s3 sync` uploads machines.json +
        # <machine>/{machine,execution,executor,application}.json + config/.
        man_dir = WORKSPACE / _MANIFEST_DIR
        if man_dir.is_dir():
            if subprocess.run([*aws, "sync", str(man_dir),
                               f"s3://{bucket}/{key}/manifest",
                               "--exclude", "*.deb", "--exclude", "BUILD.bazel"],
                              env=env).returncode != 0:
                rc_final = 1
            else:
                print(f"$ aws sync {man_dir} → s3://{bucket}/{key}/manifest/",
                      file=sys.stderr)
            # ALSO ship the manifest as ONE tarball so colony can pull it with a
            # single HTTP GET + unpack (MinIO buckets are anon-readable over HTTP
            # but not S3-LIST'able without the aws cli, and per-file get_url is
            # fragile). manifest.tar.gz = the whole tree minus debs/BUILD.
            import tarfile
            man_tgz = deb_dir / f"manifest-{key}.tar.gz"
            with tarfile.open(man_tgz, "w:gz") as tf:
                for p in sorted(man_dir.rglob("*")):
                    if p.is_file() and p.suffix != ".deb" and p.name != "BUILD.bazel":
                        tf.add(p, arcname=str(p.relative_to(man_dir)))
                # The on-device OTA assets theia authors (platform/runtime/ota/):
                # the launcher (theia-run.sh) + the Mender update-modules
                # (theia-swp/theia-app/theia-release) + state-scripts. Travel them
                # WITH the S3 manifest so colony pushes them to EVERY rig — container
                # and physical board alike — with no theia checkout on the
                # controller. colony reads them from the S3 cache (ota/…).
                ota_dir = THEIA_ROOT / "platform" / "runtime" / "ota"
                if ota_dir.is_dir():
                    # theia-run.sh at the tarball root (colony's theia_run_src
                    # default is <cache>/theia-run.sh) + ota/ for modules+scripts.
                    run_sh = ota_dir / "theia-run.sh"
                    if run_sh.is_file():
                        tf.add(run_sh, arcname="theia-run.sh")
                    for p in sorted(ota_dir.rglob("*")):
                        if p.is_file():
                            tf.add(p, arcname="ota/" + str(p.relative_to(ota_dir)))
            if subprocess.run([*aws, "cp", str(man_tgz),
                               f"s3://{bucket}/{key}/manifest.tar.gz"],
                              env=env).returncode != 0:
                rc_final = 1
            else:
                print(f"$ aws cp manifest.tar.gz → s3://{bucket}/{key}/manifest.tar.gz",
                      file=sys.stderr)
        else:
            print(f"theia release: no manifest at {man_dir} to ship — colony's "
                  "S3-pull will have nothing to seed.", file=sys.stderr)
    if pushed:
        print(f"theia release: pushed {len(pushed)} runtime .deb(s) + index "
              f"+ manifest → s3://{bucket}/", file=sys.stderr)
    return rc_final


def cmd_release(args: list[str]) -> int:
    """Release the runtime plane to S3, or build the full package set.

    TWO modes:
      theia release <target> [--s3 URL]   MANIFEST-DRIVEN — build the runtime
        debs via `theia dist <target>` and push them to the S3 runtime plane
        (s3://theia-runtime/<ver>-<abi>/). The counterpart to `theia release-swp`
        (app plane). See _release_runtime_plane. This is the S3-push verb your
        deploy chain uses.
      theia release [--arch A] [--ipk]    FULL PACKAGE BUILD (no target) — the
        legacy 4-step build below.

    The 4-step build (framework → runtime → services → package) that produces the
    ROS2-style independent packages:
      theia-framework  artheia + deps + rules → .deb (/opt/theia, setup.sh)
      theia-runtime    runtime sources + supervisor + tombstone + tdb + protos
      theia-services   com/per/sm/ucm/log/shwa binaries + services protos
      theia-rf         rf_theia harness wheel (minus scenarios/_selftest)
      (theia-tools — supervisor-GUI + rtdb — assembled when its CMake build is wired)

    Each bazel package emits a .deb (dist/debian/) — the default + primary, since
    Theia is always deployed on Debian-derived platforms. Pass `--ipk` to ALSO
    emit the embedded/opkg .ipk (dist/ipkg/) — the opt-in hatch for a non-Debian
    target. `--arch host,rpi4` builds several platforms (default: host); Python
    wheels are arch-independent (built once). `--python-only` skips the C++/bazel
    set (just the framework + rf wheels)."""
    import json   # noqa: F401  (kept for parity / future manifest reads)
    import shutil

    # NOTE: patch-versioning is an APP-PLANE concern and lives on `release-swp`
    # (`theia release-swp <app> --patch`), NOT here. `theia release` ships the
    # RUNTIME plane, which is FIXED per install — rolling it means a re-provision
    # (colony/base plane), never a free version swap. Keep runtime fixed, exchange
    # the app SWP freely: that split is the whole point (see cmd_release_swp).

    if "-h" in args or "--help" in args:
        print(cmd_release.__doc__, file=sys.stderr)
        return 0

    # MANIFEST-DRIVEN RELEASE: `theia release <target> [--s3 URL]` builds the
    # runtime plane via `theia dist <target>` (which associates a runtime manifest
    # with the theia-runtime/theia-services Deb set) and pushes it to S3. This is
    # the S3-push counterpart to the build-only `theia dist` — symmetric with
    # `theia release-swp` (the app/SWP plane). No positional target → the legacy
    # full-package build below (framework + rf wheels + runtime + dev debs).
    #
    # A bare positional is the target — but SKIP option VALUES: `release --arch
    # host` has "host" as --arch's value, NOT a target (that's the legacy full
    # build). Track which tokens are consumed by a value-taking option.
    _VALUE_OPTS = {"--arch", "--distro", "--version", "--s3", "--bucket"}
    _skip = set()
    for i, a in enumerate(args):
        if a in _VALUE_OPTS and i + 1 < len(args):
            _skip.add(i + 1)
    target = next((a for i, a in enumerate(args)
                   if not a.startswith("-") and i not in _skip), None)
    if target is not None:
        return _release_runtime_plane(target, args)

    archs = ["host"]
    # The build-distro ABI tag for the x86 services .deb Depends (com/per link
    # grpc++/libprotobuf shared). Default = the host's soname generation; pass
    # --distro ubuntu24 on a 24.04 box. Threads to --define distro=<tag> so
    # //packaging/theia:theia-services_deb selects the matching dep names.
    distro = None
    for i, a in enumerate(args):
        if a == "--arch" and i + 1 < len(args):
            archs = [x.strip() for x in args[i + 1].split(",") if x.strip()]
        if a == "--distro" and i + 1 < len(args):
            distro = args[i + 1].strip()
    for a in archs:
        if a not in _RELEASE_ARCH:
            print(f"theia release: unknown arch '{a}' "
                  f"(known: {', '.join(_RELEASE_ARCH)})", file=sys.stderr)
            return 2

    deb_dir = WORKSPACE / _DIST_DEBIAN
    ipk_dir = WORKSPACE / _DIST_IPKG
    deb_dir.mkdir(parents=True, exist_ok=True)

    python_only = "--python-only" in args
    # --runtime: build ONLY the on-device runtime+services .debs (theia-runtime +
    # theia-services), skipping the framework + rf wheels AND the -dev packages.
    # The fast path for cutting a runtime bugfix / dev rc straight to the S3 runtime
    # plane (seed-official.sh) WITHOUT a full GitHub release — the fleet provisions
    # from theia-runtime/<ver>, so only those two debs matter. Pairs with --distro.
    runtime_only = "--runtime" in args
    # .deb is the default + primary (Theia is always Debian-derived). --ipk is the
    # opt-in hatch that ALSO emits the embedded/opkg .ipk under dist/ipkg/.
    want_ipk = "--ipk" in args

    fw_out = deb_dir / "theia-framework"
    rf_out = deb_dir / "theia-rf"
    if not runtime_only:
        # ── Step 1: framework — artheia + deps + rules → a real .deb (/opt/theia,
        #    ROS2-style setup.sh). Arch-independent (Architecture: all). ──────
        fw_out.mkdir(parents=True, exist_ok=True)
        if (rc := _build_framework_deb(fw_out)) != 0:
            print("theia release: framework .deb build failed.", file=sys.stderr)
            return rc

        # ── Step 4 (python part): rf harness wheel (minus _selftest, per its
        #    pyproject find.exclude). Arch-independent. ──────────────────────────
        rf_out.mkdir(parents=True, exist_ok=True)
        if (rc := _run([sys.executable, "-m", "pip", "wheel",
                        str(WORKSPACE / "rf-theia"), "--no-deps",
                        "-w", str(rf_out)])) != 0:
            print("theia release: rf wheel build failed.", file=sys.stderr)
            return rc

        if python_only:
            print(f"theia release: python wheels → {fw_out}, {rf_out}",
                  file=sys.stderr)
            return 0

    # ── Steps 2+3: runtime + services .deb (+ .ipk with --ipk) via bazel. ─────
    distro_def = [f"--define=distro={distro}"] if distro else []
    # --runtime narrows to the two on-device debs (drop the -dev / dev-tooling pkgs).
    _RUNTIME_DEBS = {"//packaging/theia:theia-runtime_deb",
                     "//packaging/theia:theia-services_deb"}
    for arch in archs:
        platform = _RELEASE_ARCH[arch]
        deb_targets = [d for d, _ in _RELEASE_BAZEL_PKGS
                       if (not runtime_only) or d in _RUNTIME_DEBS]
        # .deb (default — per arch, emits *_amd64.deb / *_arm64.deb).
        if (rc := _run(["bazel", "build", *deb_targets,
                        f"--platforms={platform}", *distro_def])) != 0:
            return rc
        # .ipk hatch (embedded/opkg) — only with --ipk.
        if want_ipk:
            ipk_targets = [i for _, i in _RELEASE_BAZEL_PKGS if i]
            if (rc := _run(["bazel", "build", *ipk_targets,
                            f"--platforms={platform}", *distro_def])) != 0:
                return rc

    # ── Step 4 (collect): copy bazel-bin outputs into dist/. ─────────────────
    # --runtime narrows the collected set to the two on-device packages so a stale
    # framework/dev .deb lingering in bazel-bin from a prior full release isn't
    # swept into the runtime bundle. bazel outputs are read-only (r-xr-xr-x), so
    # unlink any existing dest before copying (copy2 onto a read-only file fails).
    bin_root = WORKSPACE / "bazel-bin" / "packaging" / "theia"
    _runtime_pkgs = {"theia-runtime", "theia-services"}
    n_deb = n_ipk = 0
    for f in bin_root.glob("*.deb"):
        pkg = f.name.split("_")[0]          # theia-runtime_0.1.0_amd64.deb → theia-runtime
        if runtime_only and pkg not in _runtime_pkgs:
            continue
        dst = deb_dir / pkg
        dst.mkdir(parents=True, exist_ok=True)
        dst_f = dst / f.name
        if dst_f.exists():
            dst_f.unlink()
        shutil.copy2(f, dst_f)
        # The on-device services deb carries its own per-role config defaults at
        # /opt/theia/config/<role>/ — inject them here too (the bazel pkg_deb is
        # config-free; serialize-manifest runs at release time). Same as the
        # `theia dist` path (_dist_runtime); done here so the full `theia release`
        # deb set is self-sufficient.
        if pkg == "theia-services":
            if (rc := _inject_services_config(dst_f)) != 0:
                return rc
        n_deb += 1
    if want_ipk:
        ipk_dir.mkdir(parents=True, exist_ok=True)
        for f in bin_root.glob("*.ipk"):
            pkg = f.name.split("_")[0]
            dst = ipk_dir / pkg
            dst.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, dst / f.name)
            n_ipk += 1

    msg = f"theia release: {n_deb} .deb → {deb_dir}/"
    if want_ipk:
        msg += f", {n_ipk} .ipk → {ipk_dir}/"
    print(f"{msg} (+ framework & rf wheels); arch={','.join(archs)}",
          file=sys.stderr)
    return 0


def _semver_parts(ver: str) -> "tuple[int, int, int]":
    """<ver>[-<abi>] → (major, minor, patch), abi stripped, short forms padded
    (1 → (1,0,0), 1.2 → (1,2,0)). Non-numeric component raises ValueError."""
    core = (ver or "0.0.0").split("-", 1)[0]
    parts = core.split(".")
    while len(parts) < 3:
        parts.append("0")
    return (int(parts[0]), int(parts[1]), int(parts[2]))


def _bump_patch(ver: str) -> str:
    """major.minor.PATCH → major.minor.(PATCH+1). A bare/short semver is padded
    (1 → 1.0.1, 1.0 → 1.0.1). Non-numeric PATCH raises (caller reports)."""
    major, minor, patch = _semver_parts(ver)
    return f"{major}.{minor}.{patch + 1}"


def _s3_latest_swp_version(s3_url: str, fleet: str, app: str) -> "str | None":
    """The highest published SWP semver for <app> on the package plane, ABI
    stripped (versions are stored as <ver>-<abi>). Reads
    s3://theia-swp/user-software/<fleet>/<app>/ with the aws cli. None if the aws
    cli is missing or the app has no releases yet. Used by `release-swp --patch` to
    know what to increment."""
    import os
    import subprocess
    if not shutil.which("aws"):
        return None
    bucket = os.environ.get("THEIA_SWP_BUCKET", "theia-swp")
    env = {**os.environ,
           "AWS_ACCESS_KEY_ID": os.environ.get("MINIO_USER", "theia"),
           "AWS_SECRET_ACCESS_KEY": os.environ.get("MINIO_PASSWORD", "theiaminio"),
           "AWS_DEFAULT_REGION": "us-east-1"}
    prefix = f"user-software/{fleet}/{app}/"
    # `s3 ls <prefix>` lists the version dirs as `PRE <ver>-<abi>/` lines.
    r = subprocess.run(["aws", "--endpoint-url", s3_url, "s3", "ls",
                        f"s3://{bucket}/{prefix}"],
                       env=env, capture_output=True, text=True)
    if r.returncode != 0:
        return None

    def _semver_key(v: str) -> tuple:
        # <ver>-<abi> → sort key on the numeric semver only (abi is a build flavour
        # of the SAME version, so it must not perturb the ordering).
        core = v.split("-", 1)[0]
        try:
            return tuple(int(x) for x in core.split("."))
        except ValueError:
            return (0,)

    versions = []
    for line in r.stdout.splitlines():
        line = line.strip()
        if line.startswith("PRE ") and line.endswith("/"):
            versions.append(line[4:-1])          # strip "PRE " and trailing "/"
    if not versions:
        return None
    # highest semver; strip the -abi suffix — the patch bump is on the semver, and
    # the abi is re-derived from --arch by cmd_release_swp.
    latest = max(versions, key=_semver_key)
    return latest.split("-", 1)[0]


def cmd_release_swp(args: list[str]) -> int:
    """Build + publish a USER-WS SOFTWARE PACKAGE (SWP) for day-2 Mender OTA.

    A Software Package (ARA/UCM term — see services/ucm: "binaries/libs/assets,
    via release directories") is the user's WHOLE deployable: every user-FC
    executable across ALL the SWP's compositions (one composition == one
    executable), bundled as ONE Mender overlay. It is NOT a single composition.

    The runtime/SWP dichotomy: `theia release [--runtime]` ships the UNIVERSAL
    platform (supervisor + services) to the runtime plane (colony factory-installs
    it). `theia release-swp` ships ONLY the user's FCs — the day-2 delivery unit
    Mender installs as an OVERLAY into the running release (services + the
    supervisor are NEVER touched). The SWP build is target-arch — the arch is
    DEFINED BY THE FLEET's board hardware.

    Usage:
      theia release-swp <app> [opts]    (<app> = the user's apps/<app> package)
        <app>            the apps/<app> package (e.g. gateway, apps) — the SWP's
                         compositions live under it (//apps/<app>/<Composition>)
        --swp-version V  the SWP semver (default 0.1.0) — the install dir + Mender
                         artifact name (keyed <app>-<V>-<abi>)
        --patch          AUTO-BUMP the PATCH of the app's latest published SWP
                         (1.0.0 → 1.0.1) instead of taking --swp-version. The app
                         plane is exchanged FREELY on a FIXED runtime — a PATCH is a
                         no-interface-change swap (no .art change, no migration), the
                         exact artifact a GS Rollout advances a group onto. Needs
                         --s3 to discover the latest (else --from); abi/role scoped
                         in the name = "partial per machine". Pairs with --to/--from.
        --from V         (with --patch) the base version to bump (default: the
                         highest published on S3 for this <app>/<fleet>, or 1.0.0).
        --to V           (with --patch) an explicit target version — skips the bump.
                         Crossing a MAJOR (X.0.0) is an INTERFACE / .art change: not
                         a free swap. It is REFUSED unless --migrate is also given.
        --migrate        acknowledge a MAJOR bump: an .art-level interface change.
                         Required to cross a major boundary (via --to or an explicit
                         X.0.0 --swp-version). It MAKES TWO THINGS MANDATORY:
                           • --requires-runtime <V> (the runtime pin) — refused if
                             empty (the GS gate can't protect an unpinned major app).
                           • a migration file — --migration <path>, else the
                             conventional apps/<app>/migrations/v<F>-to-v<T>.py; an
                             empty no-op stub is AUTO-CREATED if none exists (edit +
                             re-run). It ships as a package part (in the .mender +
                             synced to S3 <key>/migration/) and runs on-device during
                             ArtifactInstall, before the executor merge.
        --migration P    (with --migrate) the migration script to ship (default: the
                         conventional apps/<app>/migrations/v<F>-to-v<T>.py stub).
        --fleet F        the hardware-capability fleet / Mender device-group
                         (default theia-rig) — the S3 package-plane key + Mender
                         --device-type
        --arch A         board target (host | rpi4 | jetson; default host) — picks
                         the cross-build platform AND the abi key (rpi4=bookworm-
                         arm64, jetson=focal-arm64, host=amd64). The abi is baked
                         into the artifact-name + S3 dir so per-role Distribution
                         deploy resolves a machine to the matching-abi SWP build.
        --machine M      the manifest machine whose SWP slice to pack (default:
                         the app's host_machine from application.json)
        --s3 URL         publish to the package plane on this MinIO/S3 (e.g.
                         http://10.0.0.99:9000); omit to only build the .mender
        --mender-only    just write dist/apps/<app>/<app>-<V>.mender (no S3 push)

    Output (ABI-keyed — <V> below is <swp-version>-<abi> for cross-built boards):
      dist/apps/<app>/<app>-<V>.mender   the Mender artifact (theia-swp module)
      → S3 user-software/<fleet>/<app>/<V>/  (when --s3 given)

    The artifact payload (consumed by the on-device `theia-swp` update module):
      bin/<fc>          the SWP's FC executables (ALL compositions; the overlay)
      swp.json          {name, version, abi, roles, arity, processes[], subtree}
      version.txt       <app>-<V>  (the module's artifact-name gate)
    """
    import json
    import shutil

    if "-h" in args or "--help" in args or not args:
        print(cmd_release_swp.__doc__, file=sys.stderr)
        return 0 if args else 2

    # The <app> is the first bare positional — but SKIP the values of value-taking
    # options (e.g. `--to 2.0.0` must not make "2.0.0" the app).
    _VALUE_OPTS = {"--swp-version", "--app-version", "--fleet", "--arch", "--machine",
                   "--s3", "--from", "--to", "--requires-runtime", "--migration",
                   "--asset", "--env", "--sign-key"}
    _skip = {i + 1 for i, a in enumerate(args)
             if a in _VALUE_OPTS and i + 1 < len(args)}
    app = next((a for i, a in enumerate(args)
                if not a.startswith("-") and i not in _skip), None)
    if not app:
        print("theia release-swp: needs an <app> name (apps/<app>).",
              file=sys.stderr)
        return 2

    def _opt(name, default=None):
        for i, a in enumerate(args):
            if a == name and i + 1 < len(args):
                return args[i + 1]
        return default

    fleet = _opt("--fleet", "theia-rig")
    # --swp-version is canonical; --app-version kept as a back-compat alias.
    # --patch OVERRIDES both: auto-increment the app's latest published SWP. The app
    # plane is a FREE swap on a FIXED runtime, so a PATCH bump is the default (no
    # interface / .art change, no migration). --to gives an explicit target;
    # crossing a MAJOR is an interface change and is refused without --migrate.
    is_major = False        # a MAJOR (.art interface) bump — forces pin + migration
    if "--patch" in args:
        s3_for_latest = _opt("--s3")
        explicit_to = _opt("--to")
        base = _opt("--from")
        if explicit_to:
            app_ver = explicit_to
        else:
            if not base:
                if s3_for_latest:
                    base = _s3_latest_swp_version(s3_for_latest, fleet, app)
                if not base:
                    base = "1.0.0"
                    print(f"theia release-swp --patch: no published SWP for {app!r} "
                          f"on {fleet!r} (or no --s3/aws) — starting from {base}.",
                          file=sys.stderr)
            try:
                app_ver = _bump_patch(base)
            except ValueError:
                print(f"theia release-swp --patch: base version {base!r} has a "
                      "non-numeric component — pass --to <version> explicitly.",
                      file=sys.stderr)
                return 2
        # MAJOR guard: an .art-level interface change is NOT a free app swap. Refuse
        # to cross a major boundary (relative to the base/latest) without --migrate.
        try:
            base_major = _semver_parts(base or "0.0.0")[0]
            new_major = _semver_parts(app_ver)[0]
        except ValueError:
            base_major = new_major = 0
        is_major = new_major > base_major
        if is_major and "--migrate" not in args:
            print(f"theia release-swp --patch: {app_ver} crosses a MAJOR boundary "
                  f"(from major {base_major}). A major bump is an INTERFACE / .art "
                  "change — not a free app swap: it needs a migration + a matching "
                  "runtime (no backward compat). Re-run with --migrate to confirm.",
                  file=sys.stderr)
            return 2
        if is_major:
            print(f"theia release-swp --patch: WARNING — {app_ver} is a MAJOR bump "
                  "(.art interface change). --migrate given: a migration step + a "
                  "runtime pin are REQUIRED; this is NOT a free version swap.",
                  file=sys.stderr)
        print(f"theia release-swp: {app} {base or '?'} → v{app_ver} "
              f"({'explicit --to' if explicit_to else 'patch bump'})",
              file=sys.stderr)
    else:
        app_ver = _opt("--swp-version") or _opt("--app-version", "0.1.0")
        # --migrate WITHOUT --patch (explicit --swp-version X.0.0): the operator is
        # declaring a migration outright — treat it as a major bump (forces the
        # pin + migration file below), the same contract as the --patch path.
        is_major = "--migrate" in args
    # --arch is a BOARD TARGET (host | rpi4 | jetson), NOT a bare cpu — because the
    # app plane is ABI-keyed exactly like the runtime plane: rpi4=bookworm-arm64 and
    # jetson=focal-arm64 are different aarch64 ABIs (different sysroots, non-
    # interchangeable binaries). The Distribution model resolves a role → the app
    # build whose abi == the role's abi (see project-distribution-deploy-model), so
    # the abi MUST land in the artifact-name + S3 key, not be collapsed to aarch64.
    arch = _opt("--arch", "host")
    # The runtime release this app is built against. NO BACKWARD COMPAT — an app
    # depends on EXACTLY ONE runtime+services version (its ABI/proto/runtime are
    # pinned at build time). Recorded in app.json as `requires_runtime`; the GS
    # deploy gate refuses to install onto a device whose base_version differs.
    requires_runtime = _opt("--requires-runtime", "")
    s3_url = _opt("--s3")
    mender_only = "--mender-only" in args or not s3_url

    # ── MAJOR (.art interface change): FORCE the runtime pin + a migration part. ──
    # A major SWP is not a free swap; it depends on a specific runtime and needs a
    # migration script to move the on-device config/state across the interface
    # break. Both are MANDATORY on --migrate (the migration may be a no-op stub, but
    # it must EXIST — an explicit, reviewable, shippable artifact).
    migration_src: "Path | None" = None
    if is_major:
        # (b) FORCE requires_runtime. A major app pins EXACTLY ONE runtime; refuse to
        # ship a major with an empty pin (the GS gate can't protect an unpinned app).
        if not requires_runtime:
            print("theia release-swp --migrate: a MAJOR bump MUST pin a runtime — "
                  "pass --requires-runtime <runtime-version> (the base this app's "
                  "interface is built against). No backward compat.", file=sys.stderr)
            return 2
        # (a) require a migration file — even empty. Explicit --migration <path>, else
        # the conventional migrations/<from>-to-<to>.py; auto-CREATE an empty stub
        # (a reviewable no-op) if neither exists, so a migration ALWAYS ships.
        mig_opt = _opt("--migration")
        if mig_opt:
            migration_src = Path(mig_opt)
            if not migration_src.is_file():
                print(f"theia release-swp --migrate: --migration {mig_opt} not found.",
                      file=sys.stderr)
                return 2
        else:
            from_ver = (base if "--patch" in args else _opt("--from")) or "0"
            frm = _semver_parts(from_ver)[0] if from_ver != "0" else 0
            to = _semver_parts(app_ver)[0]
            mig_dir = WORKSPACE / "apps" / app / "migrations"
            migration_src = mig_dir / f"v{frm}-to-v{to}.py"
            if not migration_src.is_file():
                mig_dir.mkdir(parents=True, exist_ok=True)
                migration_src.write_text(
                    f'"""Migration {app} v{frm} → v{to} (MAJOR / .art interface '
                    f'change).\n\nRuns on-device during ArtifactInstall of the '
                    f'{app}-{app_ver} SWP, BEFORE the executor merge, to move config/'
                    f'\nstate across the interface break. Edit me — this is a no-op '
                    f'stub.\n\nInvoked:  python3 v{frm}-to-v{to}.py <THEIA_ROOT>\n"""\n'
                    "import sys\n\n\n"
                    "def migrate(theia_root: str) -> None:\n"
                    "    # TODO: migrate config/state for the new interface. No-op by "
                    "default.\n    pass\n\n\n"
                    'if __name__ == "__main__":\n'
                    "    migrate(sys.argv[1] if len(sys.argv) > 1 else \"/opt/theia\")\n")
                print(f"theia release-swp --migrate: created empty migration stub "
                      f"{migration_src} — EDIT it, then re-run.", file=sys.stderr)
            else:
                print(f"theia release-swp --migrate: using migration {migration_src}",
                      file=sys.stderr)
    tgt = _target(arch)
    if not tgt:
        print(f"theia release-swp: no board target for arch '{arch}' "
              f"(want one of {sorted(_TARGETS)}).", file=sys.stderr)
        return 2
    platform = _platform_label(arch)
    # The ABI suffix this app build is keyed by (bookworm-arm64 / focal-arm64; ""
    # for host/amd64). It tags the artifact-name + S3 version dir so per-role deploy
    # picks the right binary for a machine of that abi.
    abi = tgt["abi_key"]
    # The version key the artifact + S3 dir are stamped with — <ver>-<abi> when the
    # board has an abi (the cross-built rigs), bare <ver> for host.
    ver_key = f"{app_ver}-{abi}" if abi else app_ver

    # ── Resolve the app's process set + host machine from the manifest. The app's
    #    application.json (emitted by `theia manifest`) names the app + its FCs. ──
    mdir = WORKSPACE / _MANIFEST_DIR
    machine = _opt("--machine")
    app_procs: list[str] = []
    if not machine or not app_procs:
        # scan every machine's application.json for an application named <app>.
        for aj in sorted(mdir.glob("*/application.json")):
            apps = json.loads(aj.read_text()).get("applications", [])
            for a in apps:
                if a.get("name") == app:
                    machine = machine or aj.parent.name
                    app_procs = a.get("processes", [])
                    break
            if app_procs:
                break
    if not app_procs:
        print(f"theia release-swp: no application '{app}' in any "
              f"{mdir}/*/application.json — run `theia manifest` on the app rig "
              "first.", file=sys.stderr)
        return 1
    print(f"theia release-swp: {app} v{app_ver} abi={abi or 'host'} fleet={fleet} "
          f"arch={arch} machine={machine} procs={app_procs}", file=sys.stderr)

    # ── Resolve each process's bazel package prefix. A Software Package is the
    #    user's WHOLE multi-composition deliverable: an SWP spans as many
    #    compositions as it has process groups (a composition == ONE executable;
    #    the demo's Demo3WayP1..P4 are FOUR compositions in the one `apps` SWP).
    #    The canonical layout is //apps/<app>/<Composition>/main:<proc>; the
    #    executor records the "<app>/<Composition>" subtree in each process's
    #    `modules`. We build EACH proc under its own composition and stage every
    #    executable into one overlay bin/ — NOT one prefix for all (the old single-
    #    composition assumption that wrongly rejected real multi-process SWPs). ───
    ej0 = (mdir / machine / "executor.json") if machine else None
    proc_pkg: dict[str, str] = {}        # proc -> bazel PACKAGE PATH (apps/<Comp>)
    if ej0 and ej0.is_file():
        proc_pkg = _swp_proc_prefixes(json.loads(ej0.read_text()), app_procs)
    # Any proc the executor didn't pin to a composition package falls back to the
    # flat apps/<app> (legacy single-composition layout).
    for p in app_procs:
        proc_pkg.setdefault(p, f"apps/{app}")

    # ── Build every proc's FC binary (target-arch). Each composition's `main`
    #    package holds ONE cc_binary named after the cluster — gen-app names it
    #    `<app>` (e.g. //apps/Demo3WayP1/main:apps). The bazel target name is the
    #    cluster, NOT the proc; start_cmd renames the output to bin/<proc>. ────────
    cluster = app                        # gen-app's cc_binary name == the app/cluster
    fc_targets = sorted({f"//{proc_pkg[p]}/main:{cluster}" for p in app_procs})
    if (rc := _run(["bazel", "build", *fc_targets,
                    f"--platforms={platform}"])) != 0:
        print("theia release-swp: SWP FC build failed.", file=sys.stderr)
        return rc

    # ── Stage the SWP overlay tree: bin/<fc> (ALL compositions) + swp.json +
    #    version.txt. The bazel output is named <cluster>; stage it as bin/<proc>
    #    (the executor's start_cmd points at bin/<proc>). ─────────────────────────
    out_dir = WORKSPACE / "dist" / "apps" / app
    out_dir.mkdir(parents=True, exist_ok=True)
    stage = out_dir / "_stage"
    if stage.exists():
        shutil.rmtree(stage)
    (stage / "bin").mkdir(parents=True)
    for p in app_procs:
        src = WORKSPACE / "bazel-bin" / Path(proc_pkg[p]) / "main" / cluster
        if not src.is_file():
            print(f"theia release-swp: built binary missing: {src}",
                  file=sys.stderr)
            return 1
        dst = stage / "bin" / p
        shutil.copy2(src, dst)
        dst.chmod(0o755)

    # --asset <src>:<destdir> — stage extra support files into the app tree (e.g.
    # the gateway's PSP plugin: libpsp.so:psp → stage/psp/libpsp.so). The theia-app
    # module overlays the psp/ + lib/ subtrees into $CURRENT alongside bin/.
    for i, a in enumerate(args):
        if i == 0 or args[i - 1] != "--asset":
            continue
        if ":" not in a:
            print(f"theia release-swp: bad --asset '{a}' (want <src>:<destdir>).",
                  file=sys.stderr)
            return 2
        asrc, adest = a.rsplit(":", 1)
        asrc_p = Path(asrc)
        if not asrc_p.exists():
            print(f"theia release-swp: --asset source missing: {asrc_p}",
                  file=sys.stderr)
            return 1
        adest_dir = stage / adest
        adest_dir.mkdir(parents=True, exist_ok=True)
        if asrc_p.is_dir():
            shutil.copytree(asrc_p, adest_dir / asrc_p.name, dirs_exist_ok=True)
        else:
            shutil.copy2(asrc_p, adest_dir / asrc_p.name)
        print(f"theia release-swp: staged asset {asrc_p} → {adest}/", file=sys.stderr)

    # The executor subtree for just this SWP (the supervisor merges it under its
    # tree on install). Pull the SWP's slice from the machine's executor.json.
    exec_subtree = None
    ej = (mdir / machine / "executor.json") if machine else None
    if ej and ej.is_file():
        full = json.loads(ej.read_text())
        exec_subtree = _extract_swp_subtree(full, app, app_procs)
        # --env <proc>:<K>=<V> bakes per-process env into the shipped subtree (the
        # supervisor exports it to the child). The fleet's deploy knobs an FC reads
        # from the environment — e.g. gateway THEIA_GW_CAPTURE_IFACE=eth0 (the rig
        # NIC) / THEIA_GW_PSP_ROOT — that differ from the build-host defaults.
        env_specs = [a for i, a in enumerate(args)
                     if i > 0 and args[i - 1] == "--env"]
        if env_specs and exec_subtree:
            by_proc: dict = {}
            for spec in env_specs:
                if ":" not in spec or "=" not in spec.split(":", 1)[1]:
                    print(f"theia release-swp: bad --env '{spec}' "
                          "(want <proc>:<KEY>=<VAL>).", file=sys.stderr)
                    return 2
                proc, kv = spec.split(":", 1)
                k, v = kv.split("=", 1)
                by_proc.setdefault(proc, {})[k] = v
            for node in exec_subtree.get("nodes", []):
                extra = by_proc.get(node.get("name"))
                if extra:
                    node.setdefault("env", {}).update(extra)
    # ── SWP arity + roles come from the SERIALIZED MANIFEST (machines.json) — the
    #    SINGLE source, NOT a synthetic swp.json (which only duplicates it). roles
    #    = the deployment machine list, arity = len(roles), `on` = the machines the
    #    SWP's processes run on (the overlay target). See project-software-package-swp.
    mjson_path = mdir / "machines.json"
    mjson: dict = {}
    if mjson_path.is_file():
        try:
            mjson = json.loads(mjson_path.read_text())
        except Exception:  # noqa: BLE001
            mjson = {}
    roles = mjson.get("roles") or mjson.get("machines", []) or []
    arity = mjson.get("arity") or (len(roles) or 1)
    swp_on = mjson.get("on") or roles
    # The SWP NAME = the RIG name from the manifest (single / split) — the
    # deployment identity, so two rigs of the SAME app build are DISTINCT named
    # SWPs (single-… / split-…) instead of both "apps". Falls back to the bazel
    # package arg when a rig name isn't present (legacy / unnamed rig).
    swp_name = mjson.get("rig") or mjson.get("app") or app
    # SHIP THE MANIFEST in the artifact (not swp.json): machines.json + the SWP's
    # per-machine slice (machine.json/executor.json) for the boards it runs on. The
    # on-device module + GS read arity/roles/abi straight from these.
    import shutil as _sh
    (stage / "manifest").mkdir(exist_ok=True)
    _sh.copy2(mjson_path, stage / "manifest" / "machines.json")
    for mname in swp_on:
        msrc = mdir / mname
        if msrc.is_dir():
            for fn in ("machine.json", "executor.json", "application.json"):
                if (msrc / fn).is_file():
                    (stage / "manifest" / mname).mkdir(parents=True, exist_ok=True)
                    _sh.copy2(msrc / fn, stage / "manifest" / mname / fn)
    # The Mender artifact-name == the Distribution swp_build identifier == what the
    # GS per-role deploy hands Mender. ABI-keyed so central (bookworm-arm64) and
    # compute (focal-arm64) of the SAME SWP version are distinct, non-colliding
    # artifacts (e.g. gateway-1.0-bookworm-arm64 vs gateway-1.0-focal-arm64).
    artifact_name = f"{swp_name}-{ver_key}"
    (stage / "version.txt").write_text(artifact_name + "\n")
    # requires_runtime.txt — the runtime version this SWP depends on, in the
    # PAYLOAD (not just the GS index) so the on-device theia-swp module can gate
    # its install: the SWP stays pending until the installed theia-runtime deb
    # satisfies it (major.minor match, patch-tolerant). Empty = no pin (a legacy
    # / unpinned SWP installs immediately, as before).
    (stage / "requires_runtime.txt").write_text((requires_runtime or "") + "\n")
    # (c) A MAJOR SWP ships its migration as a PACKAGE PART: stage/migration/<name>
    # goes into the tarball (→ on-device payload, run during ArtifactInstall) AND is
    # synced to S3 alongside the artifact (see _publish_swp_plane). The index records
    # the filename so GS/on-device know a migration must run.
    migration_name = ""
    if migration_src and migration_src.is_file():
        (stage / "migration").mkdir(exist_ok=True)
        migration_name = migration_src.name
        _sh.copy2(migration_src, stage / "migration" / migration_name)
        print(f"theia release-swp: packed migration {migration_name} into the SWP",
              file=sys.stderr)
    # The plane-index fields the GS catalog reads, all DERIVED from the manifest.
    swp_meta = {"abi": abi, "arity": arity, "roles": roles, "on": swp_on,
                "requires_runtime": requires_runtime,
                # (d) record the migration + major flag on the index.
                "migration": migration_name, "major": bool(is_major)}

    # ── Pack the Mender artifact (theia-swp module). Reuse mender-artifact if
    #    present; else leave the staged tree + a tarball for the GW to pack. ─────
    mender_out = out_dir / f"{artifact_name}.mender"
    tarball = out_dir / f"{artifact_name}.tar.gz"
    import tarfile
    with tarfile.open(tarball, "w:gz") as tf:
        tf.add(stage, arcname=".")
    ma = _mender_artifact_bin()    # prefers the OpenSSL-1.1-shim wrapper on a host
    if ma:                          # running OpenSSL 3 (mender-artifact links 1.1).
        # SIGN the artifact (public-key authenticity, not just the S3 sha256
        # integrity). `mender-artifact write -k <priv>` RSA/ECDSA-signs; the
        # device's mender-update REFUSES any artifact not signed by the matching
        # ArtifactVerifyKey (colony ships the PUBLIC key to /etc/mender via
        # `theia cert copy`). Key resolution: --sign-key > $THEIA_SWP_SIGN_KEY >
        # the generated SWP_SIGN_KEY (`theia cert generate`, gitignored). Unsigned
        # only if none is found — warn loudly (a fleet with ArtifactVerifyKey set
        # will then reject the SWP, which is the safe failure).
        sign_key = (_opt("--sign-key")
                    or os.environ.get("THEIA_SWP_SIGN_KEY")
                    or (str(SWP_SIGN_KEY) if SWP_SIGN_KEY.is_file() else ""))
        cmd = [
            ma, "write", "module-image",
            "--type", "theia-swp",
            "--artifact-name", artifact_name,
            "--device-type", fleet,
            "--file", str(tarball),
            "--file", str(stage / "version.txt"),
            "--file", str(stage / "manifest" / "machines.json"),
            "--output-path", str(mender_out),
        ]
        if sign_key and Path(sign_key).is_file():
            cmd += ["--key", sign_key]
        rc = _run(cmd)
        if rc != 0:
            print("theia release-swp: mender-artifact pack failed.",
                  file=sys.stderr)
            return rc
        if sign_key and Path(sign_key).is_file():
            print(f"theia release-swp: wrote {mender_out} (SIGNED with "
                  f"{Path(sign_key).name})", file=sys.stderr)
        else:
            print(f"theia release-swp: wrote {mender_out} (UNSIGNED — no signing "
                  "key; a fleet with ArtifactVerifyKey set will REJECT it. Run "
                  "`theia cert generate` first, or pass --sign-key / "
                  "$THEIA_SWP_SIGN_KEY)", file=sys.stderr)
    else:
        print("theia release-swp: mender-artifact not installed — staged tree + "
              f"{tarball} written; pack on the GW.", file=sys.stderr)

    # ── Publish to the package plane: user-software/<fleet>/<app>/<ver>/. ───────
    if not mender_only and s3_url:
        # publish under the ABI-keyed version dir (user-software/<fleet>/<app>/
        # <ver>-<abi>/) so per-abi builds of one SWP version don't overwrite each
        # other and the GS catalog can offer each abi as a distinct swp_build.
        rc = _publish_swp_plane(s3_url, fleet, swp_name, ver_key,
                                mender_out if mender_out.exists() else None,
                                tarball, swp_meta)
        if rc != 0:
            return rc
    return 0


def _extract_swp_subtree(executor: dict, app: str, procs: list[str]):
    """Return the executor.json subtree (the supervisor child nodes) for just the
    SWP's processes — what the on-device SWP module merges into the running tree.
    Walks the tree, returns the matching process nodes (by name) verbatim."""
    procset = set(procs)
    found = []

    def walk(node):
        if isinstance(node, dict):
            if node.get("name") in procset and node.get("start_cmd"):
                found.append(node)
            for k in ("children", "workers", "nodes"):
                for c in node.get(k, []) or []:
                    walk(c)
    walk(executor)
    return {"app": app, "nodes": found}


def _swp_proc_prefixes(executor: dict, procs: list[str]) -> dict:
    """Map each Software-Package process to its bazel PACKAGE PATH (relative to
    the workspace root), read VERBATIM from the process node's `modules` (the
    executor records it as the package path, e.g. "apps/Demo3WayP1" or
    "apps/gateway/GatewayBridge"). The bazel label is //<package>/main:<cluster>.
    An SWP spans MANY compositions (one executable each), so different procs map
    to different packages — the canonical per-composition layout. Procs the
    executor didn't pin are absent (caller falls back to the flat apps/<app>)."""
    procset = set(procs)
    out: dict = {}

    def walk(node):
        if isinstance(node, dict):
            if node.get("name") in procset and node.get("start_cmd"):
                for m in node.get("modules", []) or []:
                    if isinstance(m, str) and "/" in m:
                        out[node["name"]] = m       # verbatim bazel package path
                        break
            for k in ("children", "workers", "nodes"):
                for c in node.get(k, []) or []:
                    walk(c)
    walk(executor)
    return out


# The mender-artifact CLI (the build host's .mender packer). Prefer the
# OpenSSL-1.1-shim wrapper (taycann runs OpenSSL 3; mender-artifact links 1.1)
# if present, else the bare binary.
def _mender_artifact_bin() -> "str | None":
    for cand in ("mender-artifact-wrap", "mender-artifact"):
        if shutil.which(cand):
            return cand
    return None


def cmd_cert(args: list[str]) -> int:
    """Manage the SWP signing keypair (app-plane authenticity).

      theia cert generate [--force] [--bits N] [--dir D]
      theia cert copy     [--s3 URL] [--bucket B]

    The app plane (the user SWP) is public-key signed: `release-swp` signs the
    .mender with the PRIVATE key; each rig verifies with the PUBLIC key (mender
    .conf ArtifactVerifyKey) and mender-update REFUSES anything not signed by it.

    `generate` writes an RSA keypair to the signing dir (deploy/signing/ by
    default, or $THEIA_SIGNING_DIR — gitignored, NEVER committed). Regenerate it
    per deployment / per nightly (an ephemeral dir keeps nothing on the box):
        THEIA_SIGNING_DIR=$(mktemp -d) theia cert generate

    `copy` uploads ONLY the PUBLIC verify key to the S3 provisioning plane
    (s3://<bucket>/provisioning/artifact-verify-key.pem); colony's provision
    pulls it onto each rig at /etc/mender/artifact-verify-key.pem. The private
    key never leaves the release host. Run generate → copy → release-swp."""
    import os
    import shutil
    import subprocess

    def _opt(name, default=None):
        for i, a in enumerate(args):
            if a == name and i + 1 < len(args):
                return args[i + 1]
        return default

    sub = args[0] if args and not args[0].startswith("-") else ""

    # honor --dir override for generate (else the module SIGNING_DIR / env)
    _dir = _opt("--dir")
    sdir = Path(_dir) if _dir else SIGNING_DIR
    priv = sdir / SWP_SIGN_KEY.name
    pub = sdir / SWP_VERIFY_KEY.name

    if sub == "generate":
        force = "--force" in args
        bits = _opt("--bits", "3072")
        if priv.is_file() and not force:
            print(f"theia cert: {priv} already exists — reuse it, or pass --force "
                  "to regenerate (INVALIDATES already-deployed verify keys).",
                  file=sys.stderr)
            return 1
        if not shutil.which("openssl"):
            print("theia cert: openssl not found.", file=sys.stderr)
            return 1
        sdir.mkdir(parents=True, exist_ok=True)
        # PKCS#8 RSA private key + the matching public key. mender-artifact signs
        # with the private (RSA-PSS); mender-update verifies with the public.
        if subprocess.run(["openssl", "genpkey", "-algorithm", "RSA",
                           "-pkeyopt", f"rsa_keygen_bits:{bits}",
                           "-out", str(priv)]).returncode != 0:
            print("theia cert: key generation failed.", file=sys.stderr)
            return 1
        os.chmod(priv, 0o600)
        if subprocess.run(["openssl", "rsa", "-in", str(priv), "-pubout",
                           "-out", str(pub)],
                          stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode != 0:
            print("theia cert: public-key extraction failed.", file=sys.stderr)
            return 1
        print(f"theia cert: generated {priv} (PRIVATE — gitignored, do NOT commit)",
              file=sys.stderr)
        print(f"theia cert: generated {pub} (PUBLIC — `theia cert copy` ships it "
              "to colony)", file=sys.stderr)
        return 0

    if sub == "copy":
        if not pub.is_file():
            print(f"theia cert: no public key at {pub} — run "
                  "`theia cert generate` first.", file=sys.stderr)
            return 1
        s3_url = _opt("--s3") or os.environ.get("THEIA_S3_URL")
        bucket = (_opt("--bucket") or os.environ.get("THEIA_RUNTIME_BUCKET")
                  or "theia-runtime")
        if not s3_url:
            print("theia cert: no --s3 URL (or $THEIA_S3_URL) — where should the "
                  "verify key go? (e.g. --s3 http://10.0.0.99:9000)",
                  file=sys.stderr)
            return 1
        if not shutil.which("aws"):
            print("theia cert: aws cli not found — push the key from a host that "
                  "has it.", file=sys.stderr)
            return 1
        env = {**os.environ,
               "AWS_ACCESS_KEY_ID": os.environ.get("MINIO_USER", "theia"),
               "AWS_SECRET_ACCESS_KEY": os.environ.get("MINIO_PASSWORD",
                                                        "theiaminio"),
               "AWS_DEFAULT_REGION": "us-east-1"}
        aws = ["aws", "--endpoint-url", s3_url, "s3"]
        subprocess.run([*aws, "mb", f"s3://{bucket}"], env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        dst = f"s3://{bucket}/{SWP_VERIFY_S3_KEY}"
        print(f"$ aws cp {pub.name} {dst}", file=sys.stderr)
        if subprocess.run([*aws, "cp", str(pub), dst], env=env).returncode != 0:
            print("theia cert: upload failed.", file=sys.stderr)
            return 1
        print(f"theia cert: copied the PUBLIC verify key → {dst}\n"
              f"  colony's provision drops it at {SWP_VERIFY_RIG_PATH} on each "
              "rig (mender.conf ArtifactVerifyKey). The private key stays here.",
              file=sys.stderr)
        return 0

    print(cmd_cert.__doc__, file=sys.stderr)
    return 2


def cmd_release_role(args: list[str]) -> int:
    """Build a per-board ROLE .mender artifact for the L4-C vehicle campaign.

    The L4-C update STORY: V-UCM fans RequestUpdate to each board's UCM with a
    BUNDLE base; each UCM resolves ITS role slice (<bundle>/<role>.mender, role =
    the board's machine name) and runs `mender install` of it. This verb builds
    that per-board role artifact: a `theia-release`-type Mender artifact carrying
    the board's runtime+services tree (/opt/theia/{bin,lib}), which the on-device
    `theia-release` update module stages as releases/<ver> + flips current.

    Usage:
      theia release-role --role R --arch A [opts]
        --role R         the board's machine/role name (e.g. central, compute) —
                         the artifact base name <role>.mender + the install slice
        --arch A         the board's target arch (rpi4 | jetson | host) — picks
                         the abi-keyed deb (bookworm-arm64 / focal-arm64)
        --version V      the release version (default 0.2.2)
        --fleet F        the Mender device-group / S3 key (default theia-rig)
        --deb PATH       the prebuilt theia-services .deb to slice (default: build
                         it for --arch). A focal-arm64 deb must be built ON the
                         jetson (native) — pass it with --deb.
        --runtime-deb P  the theia-runtime .deb (the supervisor) to fold in too.
        --s3 URL         publish to the role plane on this MinIO (the bundle base).
        --mender-only    just write dist/roles/<role>-<V>.mender (no S3 push).

    Output:
      dist/roles/<role>-<V>.mender   the role Mender artifact (theia-release module)
      → s3://theia-roles/<fleet>/<V>/<role>.mender   (when --s3 given) — the bundle
        base V-UCM passes as the manifest artifact_path."""
    import json   # noqa: F401
    import os
    import shutil as _sh
    import subprocess
    import tarfile
    import tempfile

    if "-h" in args or "--help" in args:
        print(cmd_release_role.__doc__, file=sys.stderr)
        return 0

    def _opt(name, default=None):
        for i, a in enumerate(args):
            if a == name and i + 1 < len(args):
                return args[i + 1]
        return default

    role = _opt("--role")
    arch = _opt("--arch", "rpi4")
    ver = _opt("--version", "0.3.0")
    fleet = _opt("--fleet", "theia-rig")
    s3_url = _opt("--s3")
    services_deb = _opt("--deb")
    runtime_deb = _opt("--runtime-deb")
    mender_only = "--mender-only" in args or not s3_url
    if not role:
        print("theia release-role: --role <board> required.", file=sys.stderr)
        return 2

    ma = _mender_artifact_bin()
    if not ma:
        print("theia release-role: mender-artifact not found — install it (or the "
              "mender-artifact-wrap OpenSSL-1.1 shim) to pack the .mender.",
              file=sys.stderr)
        return 1

    # ── Resolve / build the board's services deb (the role payload source). ────
    if not services_deb:
        platform = _RELEASE_ARCH.get(arch)
        if not platform:
            print(f"theia release-role: unknown arch '{arch}'.", file=sys.stderr)
            return 2
        if (rc := _run(["bazel", "build", "//packaging/theia:theia-services_deb",
                        "//packaging/theia:theia-runtime_deb",
                        f"--platforms={platform}"])) != 0:
            return rc
        bin_root = WORKSPACE / "bazel-bin" / "packaging" / "theia"
        deb_arch = _target(arch).get("deb_arch", "amd64")
        services_deb = str(bin_root / f"theia-services_{ver}_{deb_arch}.deb")
        runtime_deb = runtime_deb or str(bin_root / f"theia-runtime_{ver}_{deb_arch}.deb")
    if not Path(services_deb).is_file():
        print(f"theia release-role: services deb not found: {services_deb}\n"
              "  (a focal-arm64 role must be built ON the jetson; pass --deb.)",
              file=sys.stderr)
        return 1

    out_dir = WORKSPACE / "dist" / "roles"
    out_dir.mkdir(parents=True, exist_ok=True)
    artifact_name = f"{role}-{ver}"

    with tempfile.TemporaryDirectory() as td:
        tdp = Path(td)
        rel = tdp / "release"
        rel.mkdir()
        for deb in (services_deb, runtime_deb):
            if deb and Path(deb).is_file():
                subprocess.run(["dpkg-deb", "-x", deb, str(tdp / "x")], check=True)
        src = tdp / "x" / "opt" / "theia"
        if not src.is_dir():
            print("theia release-role: deb has no /opt/theia tree.", file=sys.stderr)
            return 1
        for sub in ("bin", "lib"):
            if (src / sub).is_dir():
                _sh.copytree(src / sub, rel / sub)
        (tdp / "version.txt").write_text(artifact_name + "\n")
        tarball = tdp / "release.tar.gz"
        with tarfile.open(tarball, "w:gz") as tf:
            tf.add(rel, arcname=".")

        mender_out = out_dir / f"{artifact_name}.mender"
        rc = _run([ma, "write", "module-image",
                   "--type", "theia-release",
                   "--artifact-name", artifact_name,
                   "--device-type", fleet,
                   "--file", str(tarball),
                   "--file", str(tdp / "version.txt"),
                   "--output-path", str(mender_out)])
        if rc != 0:
            print("theia release-role: mender-artifact pack failed.", file=sys.stderr)
            return rc
        print(f"theia release-role: wrote {mender_out} "
              f"(role={role} arch={arch} v{ver})", file=sys.stderr)

        if not mender_only and s3_url:
            if not shutil.which("aws"):
                print("theia release-role: aws cli not found — built the artifact; "
                      "push from a host with aws.", file=sys.stderr)
                return 1
            env = {**os.environ,
                   "AWS_ACCESS_KEY_ID": os.environ.get("MINIO_USER", "theia"),
                   "AWS_SECRET_ACCESS_KEY": os.environ.get("MINIO_PASSWORD", "theiaminio"),
                   "AWS_DEFAULT_REGION": "us-east-1"}
            bucket = "theia-roles"
            key = f"{fleet}/{ver}"
            aws = ["aws", "--endpoint-url", s3_url, "s3"]
            subprocess.run([*aws, "mb", f"s3://{bucket}"], env=env)
            dst = f"s3://{bucket}/{key}/{role}.mender"
            print(f"$ aws cp {mender_out.name} {dst}", file=sys.stderr)
            if subprocess.run([*aws, "cp", str(mender_out), dst],
                              env=env).returncode != 0:
                return 1
            print(f"theia release-role: published → {dst}", file=sys.stderr)
    return 0


def _publish_swp_plane(s3_url: str, fleet: str, app: str, ver: str,
                       mender: "Path | None", tarball: "Path",
                       swp_meta: dict) -> int:
    """Push the Software Package to the S3 package plane
    user-software/<fleet>/<app>/<ver>/. Uses the aws cli (S3-compatible) against
    MinIO. Writes an index.json the GW / ground-station reads to drive a Mender
    deployment. `swp_meta` (abi/arity/roles/on/requires_runtime) is DERIVED from
    the serialized manifest by the caller — the index needs no swp.json."""
    import json
    import os
    import subprocess
    if not shutil.which("aws"):
        print("theia release-swp: aws cli not found — cannot push to S3 (build "
              "the artifact, push from a host that has it).", file=sys.stderr)
        return 1
    key = f"user-software/{fleet}/{app}/{ver}"
    env = {**os.environ,
           "AWS_ACCESS_KEY_ID": os.environ.get("MINIO_USER", "theia"),
           "AWS_SECRET_ACCESS_KEY": os.environ.get("MINIO_PASSWORD", "theiaminio"),
           "AWS_DEFAULT_REGION": "us-east-1"}
    # The package plane bucket. (Renamed from theia-apps → theia-swp with the
    # app→Software-Package rename; GS settings.s3_swp_bucket reads the same.)
    bucket = os.environ.get("THEIA_SWP_BUCKET", "theia-swp")
    aws = ["aws", "--endpoint-url", s3_url, "s3"]

    def _aws(argv: list, check: bool = True) -> int:
        print(f"$ {' '.join(argv)}", file=sys.stderr)
        return subprocess.run(argv, env=env).returncode if not check else \
            subprocess.run(argv, env=env, check=False).returncode

    subprocess.run([*aws, "mb", f"s3://{bucket}"], env=env)  # idempotent
    objs = []
    for f in (mender, tarball):
        if f and f.is_file():
            if _aws([*aws, "cp", str(f), f"s3://{bucket}/{key}/{f.name}"]) != 0:
                return 1
            objs.append(f.name)
    # The index carries the SWP fields the GS catalog reads (abi/arity/roles/on +
    # requires_runtime) — all DERIVED from the serialized manifest by the caller,
    # so the catalog needs neither a swp.json nor a fetch of the artifact.
    roles = swp_meta.get("roles", []) or []
    idx = {"plane": "swp", "fleet": fleet, "app": app, "version": ver,
           "artifact": (mender.name if mender and mender.is_file() else None),
           "requires_runtime": swp_meta.get("requires_runtime", ""),
           "abi": swp_meta.get("abi", ""),
           "arity": swp_meta.get("arity", len(roles) or 1),
           "roles": roles, "on": swp_meta.get("on", roles),
           # A MAJOR SWP records its migration part + the flag so GS/on-device know
           # a migration must run (and that this is not a free swap).
           "migration": swp_meta.get("migration", ""),
           "major": bool(swp_meta.get("major", False)),
           "files": objs}
    # Write the index alongside the artifact (tarball.parent is the staged
    # dist/apps/<pkg>/ dir) — `app` here is the SWP/rig name, which may differ from
    # the on-disk package dir, so don't rebuild the path from it.
    idx_path = (tarball.parent if tarball else WORKSPACE) / f"index-{ver}.json"
    idx_path.write_text(json.dumps(idx, indent=2))
    if _aws([*aws, "cp", str(idx_path), f"s3://{bucket}/{key}/index.json"]) != 0:
        return 1
    # ── The per-machine MANIFEST as SEPARATE S3 objects (not only inside the
    # tarball). colony pulls <key>/manifest/<machine>/ directly from S3 to seed a
    # board's executor + config, symmetric with the runtime plane — so an operator
    # (or colony) can read the deploy shape without unpacking the .mender. The
    # tarball's sibling _stage/manifest holds machines.json + per-machine slices.
    stage_manifest = (tarball.parent / "_stage" / "manifest") if tarball else None
    if stage_manifest and stage_manifest.is_dir():
        if _aws([*aws, "sync", str(stage_manifest),
                 f"s3://{bucket}/{key}/manifest"]) != 0:
            return 1
        print(f"$ aws sync {stage_manifest} → s3://{bucket}/{key}/manifest/",
              file=sys.stderr)
    # ── The MIGRATION as a SEPARATE S3 object (a package part), so GS/an operator
    # can read/fetch it without unpacking the .mender — mirrors the manifest sync.
    stage_migration = (tarball.parent / "_stage" / "migration") if tarball else None
    if stage_migration and stage_migration.is_dir():
        if _aws([*aws, "sync", str(stage_migration),
                 f"s3://{bucket}/{key}/migration"]) != 0:
            return 1
        print(f"$ aws sync {stage_migration} → s3://{bucket}/{key}/migration/",
              file=sys.stderr)
    print(f"theia release-swp: published → s3://{bucket}/{key}/ "
          "(artifact + index + manifest)", file=sys.stderr)
    return 0


# Default scope for the compile DB: the in-tree, LINUX-buildable C++ trees
# clangd needs to index. Kept explicit (not `//...`) so the aquery doesn't
# drag in (a) vendor 3pp / external repos with their own build systems, or
# (b) //gateway/firmware (the Hercules TMS570 firmware — a different
# toolchain, --config=ti_arm_cgt_18, that fails under --config=linux). Only
# the linux gateway libs (//gateway/libs) belong here. Override by passing
# target patterns after the verb (e.g. `theia compdb //services/...`).
_COMPDB_TARGETS = [
    "//services/...",
    "//platform/...",
    "//apps/...",
    "//gateway/libs/...",
]


def cmd_compdb(args: list[str]) -> int:
    """Regenerate compile_commands.json from Bazel (for clangd).

    Runs `bazel aquery mnemonic(CppCompile, <targets>)` and writes the
    {file, arguments, directory} entries to compile_commands.json at the
    workspace root. Pass target patterns to narrow the scope; default
    covers services/platform/apps/gateway. Pass `--config=<x>` (or any
    bazel flag starting with -) and it's forwarded to the aquery.
    """
    import json
    import shlex

    targets = [a for a in args if not a.startswith("-")] or _COMPDB_TARGETS
    bazel_flags = [a for a in args if a.startswith("-")]
    if not any(f.startswith("--config") for f in bazel_flags):
        bazel_flags.append("--config=linux")

    pattern = " union ".join(targets)
    aquery = [
        "bazel", "aquery", *bazel_flags,
        f'mnemonic("CppCompile", {pattern})',
        "--output=jsonproto",
        "--include_artifacts=false",  # we read the source from the args
        # Defensive: tolerate any package that fails to load/analyze, indexing
        # everything that DID. (The original offender — services/log:all_srcs +
        # com referencing the retired //platform/supervisor:proto_srcs over a
        # CMake/grpc edge — is GONE: log is a clean gen-app FC and com's trace
        # gRPC moved to TraceForwarder. //services/... loads clean now. Kept as
        # a safety net so a future broken package degrades to a partial compdb
        # instead of aborting the whole regen.)
        "--keep_going",
    ]
    print(f"$ {' '.join(shlex.quote(a) for a in aquery)}", file=sys.stderr)
    try:
        proc = subprocess.run(aquery, capture_output=True, text=True)
    except FileNotFoundError as e:
        print(f"theia: {e}", file=sys.stderr)
        return 127
    if proc.returncode not in (0, 1):
        # rc >= 2 is a real invocation error (bad flag, missing bazel, query
        # syntax) — bail. rc 1 is --keep_going's "not all targets analyzed"
        # partial, which still writes valid jsonproto; fall through to it.
        sys.stderr.write(proc.stderr)
        return proc.returncode
    if proc.returncode == 1:
        # --keep_going partial: some targets failed to load/analyze. Show
        # bazel's reason (which packages) but proceed with what DID analyze.
        sys.stderr.write(proc.stderr)
        print("theia: compdb is PARTIAL — some targets were skipped "
              "(see errors above); regenerated from the rest.",
              file=sys.stderr)

    try:
        data = json.loads(proc.stdout)
    except json.JSONDecodeError as e:
        print(f"theia: aquery produced no/invalid jsonproto: {e}",
              file=sys.stderr)
        return 1

    _SRC_EXT = (".cc", ".cpp", ".cxx", ".c", ".c++")
    seen: set[str] = set()
    entries: list[dict] = []
    for act in data.get("actions", []):
        argv = act.get("arguments", [])
        # The source operand is the first non-output translation unit in
        # the compile args (outputs end in .o / .pic.o).
        src = next((a for a in argv
                    if a.endswith(_SRC_EXT) and not a.endswith(".o")), None)
        if not src or src in seen:
            continue
        seen.add(src)
        entries.append({
            "file": src,
            "arguments": argv,
            "directory": str(WORKSPACE),
        })

    out = WORKSPACE / "compile_commands.json"
    out.write_text(json.dumps(entries, indent=1) + "\n")
    print(f"theia: wrote {len(entries)} entries to {out}", file=sys.stderr)
    return 0


def _py_ident_safe(name: str) -> str:
    """A bazel-module-name-safe identifier from a workspace name (lower, digits,
    underscores; leading non-alpha gets an `m` prefix)."""
    s = "".join(c if (c.isalnum() or c == "_") else "_" for c in name.lower())
    return s if (s and (s[0].isalpha() or s[0] == "_")) else f"m_{s}"


def _read_or(path: "Path", default: str) -> str:
    try:
        return path.read_text()
    except OSError:
        return default + "\n"


def cmd_init(args: list[str]) -> int:
    """Scaffold the CURRENT directory as a Theia consuming workspace.

    The catkin-`catkin init` / ROS-`colcon` analogue: turns an empty repo into
    a workspace that builds apps against a SIBLING Theia source checkout (or an
    installed /opt/theia prefix later). Run it from your workspace root after
    sourcing the framework — `source ../theia/env.sh` for a source checkout, or
    `source /opt/theia/setup.sh` for the installed deb:

        cd ~/repo/launch-box/gataway_ws
        source ../theia/env.sh             # exports THEIA_ROOT (source checkout)
        theia init [--name <ws>]           # bare workspace (supervisor + your apps)
        theia init --with-services         # + the ARA services (com/log/per/sm/ucm/shwa)
        theia init --kind package --name <X>   # a reusable PACKAGE repo (node +
                                               # protocol + impl → src/{lib,impl}),
                                               # NOT a workspace — see _init_package

    --with-services bootstraps the workspace with the platform services: it links
    system/services and emits a rig built on the framework's ServicesSoftware, so
    a bare `theia start` brings the full service tree up under the supervisor.

    It creates, in the CWD (never overwriting an existing file):
      - system/<name>/{package,component}.art — this workspace's OWN app package,
        named after --name (FQN system.<name> ↔ this path 1:1; no symlink). <name>
        is the SWP/app name, propagated through the source package, the deploy dir,
        and the manifest target. You edit these.
      - system/system.art   — the workspace aggregator (imports system.<name>.*).
        You `theia manifest <name>` against it.
      - manifest/<name>/rig.py — the one-machine deploy rig, addressable as
        `theia manifest <name>`; imports the generated app manifest (gen-manifest
        writes manifest/<name>/manifest.py).
      - apps/, proto/       — homes for the GENERATED C++ (gen-app --out apps →
        //apps/<Comp>/main) and proto (--proto-out proto); never mixed with the fwk.
      - .theia               — records THEIA_ROOT (the source it's bound to).

    Re-runnable: `theia init` is idempotent — it never overwrites your files
    (system/apps, impl/), only (re)links the framework deps + (re)writes the
    scaffold BUILD/shim files. Run it again (e.g. add --with-services) any time.

    A BARE workspace plants NO framework symlinks — its system.art imports only
    system.apps.*, and the supervisor is staged from the fixed framework target at
    manifest/install time. Only --with-services vendors the framework .art it
    imports (system/{platform/runtime,supervisor,services,platform/msgs}) as
    SYMLINKS into $THEIA_ROOT, so a Theia bump is a re-source, not a re-copy."""
    if "-h" in args or "--help" in args:
        print(cmd_init.__doc__, file=sys.stderr)
        return 0

    theia_root = os.environ.get("THEIA_ROOT")
    if not theia_root:
        print("theia init: THEIA_ROOT is unset — `source /path/to/theia/env.sh` "
              "(source checkout) or `source /opt/theia/setup.sh` (deb) first so "
              "the workspace knows where the Theia framework lives.",
              file=sys.stderr)
        return 2
    theia_root = Path(theia_root).resolve()
    # THEIA_ROOT is either a SOURCE checkout or an INSTALLED prefix (/opt/theia
    # from the debs) — but both now share ONE FQN-mirrored on-disk layout under
    # system/ (the deb ships the services .art at /opt/theia/system/services via
    # theia-services-dev, matching the source tree), so there's a single set of
    # paths. Each mirrors its package FQN 1:1 (the runtime is `package
    # system.platform.runtime` → system/platform/runtime/). We plant workspace
    # symlinks at these so the artheia resolver reaches the real files.
    runtime_pkg = theia_root / "system" / "platform" / "runtime"
    runtime_art = runtime_pkg / "package.art"
    supervisor_pkg = theia_root / "system" / "supervisor"
    services_pkg = theia_root / "system" / "services"
    msgs_pkg = theia_root / "system" / "platform" / "msgs"
    if not runtime_art.is_file():
        print(f"theia init: THEIA_ROOT={theia_root} doesn't look like a Theia "
              "source checkout OR an installed /opt/theia prefix "
              f"(no runtime package.art at {runtime_art}). Install the "
              "theia-runtime-dev deb, or source a source checkout's env.sh.",
              file=sys.stderr)
        return 2

    # The workspace to scaffold is the CALLER's cwd (main() chdir'd to the Theia
    # checkout before dispatch; THEIA_INVOCATION_CWD preserves where we started).
    ws = Path(os.environ.get("THEIA_INVOCATION_CWD", Path.cwd())).resolve()
    name = ws.name
    kind = "ws"
    for i, a in enumerate(args):
        if a == "--name" and i + 1 < len(args):
            name = args[i + 1]
        elif a == "--kind" and i + 1 < len(args):
            kind = args[i + 1]
    if kind not in ("ws", "package"):
        print(f"theia init: --kind must be ws|package (got {kind!r}).",
              file=sys.stderr)
        return 2
    # --with-services: bootstrap with the ARA platform services (com/log/per/sm/
    # ucm/shwa). Links system/services + the rig builds on ServicesSoftware, so a
    # bare `theia start` brings the full service tree up under the supervisor.
    with_services = "--with-services" in args
    if theia_root == ws:
        print("theia init: refusing to init the Theia checkout itself "
              "(run from your CONSUMING workspace dir).", file=sys.stderr)
        return 2

    # --kind package: a ROS-style PACKAGE repo (nodes + protocol + impl as a
    # composable unit + its own probe test), scaffolded so the WHOLE toolchain
    # runs unmodified: gen-app --kind package (→impl), gen-app --kind fc
    # component.art (→apps, gitignored), gen-manifest, serialize-manifest,
    # theia install/start, and `robot test/<name>.robot` (the probe drives the
    # node's ctl in isolation). A workspace CONSUMES packages; a package is its
    # own repo. See docs/tasks/BACKLOG/theia-packages.md.
    if kind == "package":
        return _init_package(ws, theia_root, name, with_services)

    created: list[str] = []
    # --name drives the app's identity end-to-end: the SWP name, the source
    # package (system/<slug>, FQN system.<slug>), the manifest/rig target
    # (manifest/<slug> → `theia manifest <slug>`), and the composition name
    # (<Cls>, CamelCase → apps/<Cls>/main). `slug` = py/bazel-safe form of --name;
    # `Cls` = CamelCase for the composition + node class.
    slug = _py_ident_safe(name)
    Cls = "".join(p.capitalize() for p in name.replace("-", "_").split("_"))

    def _sub(t: str) -> str:
        return t.replace("@NAME@", slug).replace("@CLS@", Cls)

    def _write(rel: str, content: str) -> None:
        p = ws / rel
        if p.exists():
            print(f"theia init: keep existing {rel}", file=sys.stderr)
            return
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
        created.append(rel)

    def _link(rel: str, target: Path) -> None:
        p = ws / rel
        if p.exists() or p.is_symlink():
            print(f"theia init: keep existing {rel}", file=sys.stderr)
            return
        p.parent.mkdir(parents=True, exist_ok=True)
        # Relative symlink so the workspace stays relocatable as a pair with theia.
        rel_target = os.path.relpath(target, p.parent)
        p.symlink_to(rel_target)
        created.append(f"{rel} -> {rel_target}")

    def _sync_pin(rel: str, content: str) -> None:
        """A framework-PINNED build file (.bazelversion): it must MATCH the
        framework, so re-sync it on mismatch rather than 'keep existing'. A stale
        .bazelversion from an earlier init against an older framework causes an
        incompatible bazel to load the framework's toolchain configs — e.g.
        `name 'set' is not defined` in rules_cc's cc_toolchain_config, aborting the
        build. This is generated config, not user-edited source, so overwriting is
        safe (unlike .art)."""
        p = ws / rel
        try:
            existing = p.read_text() if p.exists() else None
        except OSError:
            existing = None
        if existing is not None and existing.strip() == content.strip():
            return                       # already correct — silent
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
        created.append(rel + ("  (re-synced to framework)" if existing else ""))

    # The supervisor is a DEPLOY-time fabric: `theia manifest`/`install` stage it
    # from the FIXED framework target (//platform/supervisor/main:supervisor,
    # $THEIA_ROOT), NOT from this workspace's ART. A BARE workspace's system.art
    # therefore imports ONLY system.apps.* — no system.supervisor / system.platform
    # reference — so it needs neither the system/supervisor nor the
    # system/platform/runtime symlink. (Verified: parse → manifest → install →
    # start a bare supervisor all work without them.) Only --with-services, whose
    # service .art DOES import system.platform.runtime.* + system.platform.msgs.* +
    # system.services.*, needs the framework symlinks so those FQNs resolve.
    if with_services:
        # runtime: services import system.platform.runtime.* (ChildControlIf,
        # TraceControlPush, LogLevelPush) — resolver maps the FQN to
        # system/platform/runtime/.
        _link("system/platform/runtime", runtime_pkg)
        # supervisor: the services aggregator imports system.supervisor.* (the tree
        # that hosts the service FCs); link it so that FQN resolves.
        if supervisor_pkg.exists():
            _link("system/supervisor", supervisor_pkg)
        # the framework's ARA service FCs so `cluster Services` resolves in the .art
        # tree. (The rig gets the services MANIFEST by path-load from
        # $THEIA_ROOT/manifest/, not from this .art link.)
        _link("system/services", services_pkg)
        # service .art import system.platform.msgs.{std,geometry,sensor,nav}.* (e.g.
        # tsync uses nav.GnssSolution) — one link covers all four subpackages
        # (FQN system.platform.msgs.<x> → msgs/<x>/).
        if msgs_pkg.exists():
            _link("system/platform/msgs", msgs_pkg)
    # The workspace's OWN empty app package (no compositions yet). gen-manifest
    # walks `cluster Applications` here — empty → an empty app manifest +
    # executor sidecar, which the rig imports as-is.
    #
    # The app is named after --name: system/<name> is the REAL, canonical app
    # source (FQN system.<name> maps to the dir 1:1 — no indirection, no symlink)
    # and <name> is this workspace's SWP/app name. The user edits these; gen-app
    # emits the C++ to apps/<Composition>/ (gen-app --out apps → //apps/<Comp>/main,
    # the demo convention) and the proto to proto/, gen-manifest writes the Python
    # sidecar to manifest/<name>/ — all SEPARATE from this source dir. (`slug` = a
    # py/bazel-safe form of --name for dir + module + label use.)
    _write(f"system/{slug}/package.art", _sub(_INIT_APPS_PACKAGE_ART))
    _write(f"system/{slug}/component.art", _sub(_INIT_APPS_COMPONENT_ART))
    # Python package marker for the generated C++ tree (apps/). NOTE: `manifest/`
    # here is the WORKSPACE's own manifest package (manifest.<name> / the rig),
    # local to this dir. The framework's services manifest is NOT part of this
    # package — the rig loads it by path from $THEIA_ROOT/manifest/ — so there's no
    # cross-root namespace-package coupling to preserve.
    _write("apps/__init__.py", "")

    sys_art = (_INIT_SYSTEM_ART_SERVICES if with_services else _INIT_SYSTEM_ART)
    rig_py = (_INIT_RIG_PY_SERVICES if with_services else _INIT_RIG_PY)
    _write("system/system.art", _sub(sys_art))
    # The deploy rig — a one-machine smoke-test target for verifying a fresh
    # workspace's toolchain. Named after --name (manifest/<name>/rig.py) so it's
    # addressable as `theia manifest <name>` (manifest.<target>.rig); real
    # per-target rigs a workspace grows later sit beside it.
    _write(f"manifest/{slug}/__init__.py", "")
    _write(f"manifest/{slug}/rig.py", _sub(rig_py))
    # Record THEIA_ROOT RELATIVE to the workspace when they share a prefix (keeps
    # the ws+theia pair relocatable, e.g. a workspace living next to the
    # framework checkout with a `../` link); fall back to absolute if they're
    # on different roots.
    try:
        _theia_root_rec = os.path.relpath(theia_root, ws)
    except ValueError:
        _theia_root_rec = str(theia_root)
    _write(".theia", f"THEIA_ROOT={_theia_root_rec}\nname={name}\n")
    # setup_local.sh — the workspace activation shim: `source <ws>/setup_local.sh`
    # from anywhere pins THEIA_WORKSPACE to this dir + sources the framework. The
    # deploy tooling (theia provision/orchestrate) reads $THEIA_WORKSPACE/deploy/
    # {registry,config}/, so a rig's data lives in ITS workspace, never the fwk.
    _write("setup_local.sh", _INIT_SETUP_LOCAL.replace("@NAME@", name))
    _setup_local = ws / "setup_local.sh"
    if _setup_local.exists():
        _setup_local.chmod(0o755)
    # .mcp.json — wire the MCP servers for a session in THIS workspace. The
    # theia/artheia/rf-theia servers fix their workspace to the launch cwd, so
    # they must run FROM here; artheia/rf-theia live in the workspace venv, the
    # theia server + work-with-me ship under $THEIA_ROOT (the deb's /opt/theia).
    _write(".mcp.json", _INIT_MCP_JSON.replace("@THEIA_ROOT@", str(theia_root)))
    # deploy/ homes for this workspace's rig data (registry + per-rig config
    # overrides). Empty + a .gitkeep so the dirs exist for the operator to drop
    # <rig>.yml / <rig>/<fc>.json into; the framework playbooks read them via
    # $THEIA_WORKSPACE/deploy/.
    _write("deploy/registry/.gitkeep", "")
    _write("deploy/config/.gitkeep", "")
    _write("README.md", _INIT_README.replace("@NAME@", name)
                                   .replace("@THEIA_ROOT@", str(theia_root)))

    # ── Bazel module: the workspace builds its OWN app C++ against the sibling
    # Theia (the gataway_ws pattern). Its own MODULE.bazel consumes pero_theia
    # via local_path_override; gen-app emits the framework labels already
    # qualified as @pero_theia//platform/... (it detects the consuming-workspace
    # layout), so they resolve directly against the sibling module — NO per-
    # workspace alias shims to write. The app's OWN proto
    # (//proto:platform_protos → system/apps) builds LOCALLY so a workspace whose
    # .art differs from the framework's gets its own wire types. Without this a
    # pure consumer fell back to building from THEIA_ROOT (wrong source tree). ──
    _mod_name = _py_ident_safe(name)
    # local_path_override path: relative ws→theia (keeps the ws+theia pair
    # relocatable), abs fallback if they share no useful common prefix.
    try:
        _theia_rel = os.path.relpath(theia_root, ws)
    except ValueError:
        _theia_rel = str(theia_root)
    _write("MODULE.bazel", _INIT_MODULE_BAZEL.replace("@MODNAME@", _mod_name)
                                             .replace("@THEIA_REL@", _theia_rel))
    _write(".bazelrc", _INIT_BAZELRC)
    # .bazelversion MUST match the framework's — an incompatible bazel can't load
    # @pero_theia's toolchain configs. Re-sync (not keep-existing) so a re-init
    # against a bumped framework picks up the new pin. Default falls back to the
    # framework's current pin if the file is somehow unreadable.
    _sync_pin(".bazelversion", _read_or(theia_root / ".bazelversion", "9.1.0"))
    # The app's own proto package: gen-app writes <slug>.proto + <slug>.options
    # under proto/system/<slug>/ (--proto-out proto keys the subpath off the FQN
    # system.<slug>); this BUILD nanopb-compiles them. //proto:platform_protos
    # aggregates it (+ the runtime proto from @pero_theia) so the gen-app lib's
    # `//proto:platform_protos` dep resolves locally. The app proto lives under
    # proto/ (the workspace's own), NOT platform/proto/ — they never mix.
    _write("proto/BUILD.bazel", _sub(_INIT_PROTO_AGG))
    _write(f"proto/system/{slug}/BUILD.bazel", _sub(_INIT_APPS_PROTO_BUILD))

    flavour = "services workspace" if with_services else "empty workspace"
    print(f"\ntheia init: scaffolded '{name}' ({flavour}) against {theia_root}",
          file=sys.stderr)
    for c in created:
        print(f"  + {c}", file=sys.stderr)
    extra = ("\n  (the ARA services com/log/per/sm/ucm/shwa come up under the "
             "supervisor)" if with_services else "")
    print(f"\nThe scaffold ships a runnable placeholder app ('{Cls}'). Run the "
          f"toolchain end to end:\n"
          f"  # 1. generate the app C++ (→ apps/{Cls}/) + proto:\n"
          f"  artheia gen-app --kind fc system/{slug}/component.art "
          f"--out apps --proto-out proto\n"
          f"  # 2. manifest + install + run (theia manifest <{slug}>, the rig):\n"
          f"  artheia gen-manifest system/{slug}/component.art "
          f"manifest/{slug}/manifest.py\n"
          f"  theia manifest {slug} && theia install {slug} && theia start{extra}\n"
          f"  bazel build //apps/...        # (or let theia install build it)\n"
          f"\nThen edit system/{slug}/package.art (your nodes) + the write-once "
          f"apps/{Cls}/impl/{Cls}Node_handlers.cc (your handler bodies).",
          file=sys.stderr)
    return 0


def _init_package(ws: Path, theia_root: Path, name: str,
                  with_services: bool) -> int:
    """Scaffold the CWD as a ROS-style Theia PACKAGE repo (theia init --kind
    package). A package is a self-contained, independently-repo'd, composable
    unit — nodes + protocol + impl + its own probe test — that a workspace
    consumes. Everything is parameterized by `name` so the whole toolchain runs
    on a FRESH scaffold with zero edits:

        gen-app --kind package system/<name>/package.art --out src --ns ara::<name> → src/{lib,impl}
        gen-app --kind fc      system/<name>_tester/component.art --out apps → apps/ (gitignored)
        gen-manifest / serialize-manifest (from manifest/rig.py)
        theia install / theia start
        robot test/<name>.robot   (the probe drives the node's ctl in isolation)
    """
    import os as _os
    slug = _py_ident_safe(name)
    Cls = "".join(p.capitalize() for p in name.replace("-", "_").split("_"))
    created: list[str] = []

    def _write(rel: str, content: str) -> None:
        p = ws / rel
        if p.exists():
            print(f"theia init: keep existing {rel}", file=sys.stderr)
            return
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
        created.append(rel)

    def _sync_pin(rel: str, content: str) -> None:
        """Framework-PINNED build file (.bazelversion): re-sync on mismatch (a
        stale pin loads an incompatible bazel against @pero_theia's toolchains).
        See the twin in _init_ws for the full rationale."""
        p = ws / rel
        try:
            existing = p.read_text() if p.exists() else None
        except OSError:
            existing = None
        if existing is not None and existing.strip() == content.strip():
            return
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
        created.append(rel + ("  (re-synced to framework)" if existing else ""))

    def _link(rel: str, target: Path) -> None:
        p = ws / rel
        if p.exists() or p.is_symlink():
            return
        p.parent.mkdir(parents=True, exist_ok=True)
        p.symlink_to(_os.path.relpath(target, p.parent))
        created.append(f"{rel} -> {_os.path.relpath(target, p.parent)}")

    def _sub(t: str) -> str:
        return (t.replace("@NAME@", name).replace("@SLUG@", slug)
                 .replace("@CLS@", Cls))

    # NO framework symlinks. A package's .art never imports system.supervisor /
    # system.platform — the supervisor is a DEPLOY-time fabric (it forks + runs the
    # app), resolved from $THEIA_ROOT by `theia manifest`, not referenced from ART.
    # (--with-services still links system/services since the tester CAN import an
    # ARA service interface; only the never-referenced supervisor/platform go.)
    if with_services:
        _link("system/services", theia_root / "system" / "services")
        if (theia_root / "system" / "platform" / "msgs").exists():
            _link("system/platform/msgs", theia_root / "system" / "platform" / "msgs")

    # TWO packages, TWO dirs under system/ (FQN mirrors the dir 1:1 — no tautological
    # `packages/` prefix, no symlinks). This models the ROS import/link relationship
    # in ONE workspace, exactly as an EXTERNAL consumer would:
    #
    #   system/<name>/package.art          `package system.<name>` — the NODE
    #     → gen-app --kind package --out system/<name> → system/<name>/{lib,impl}
    #       (a linkable //system/<name>/lib:<name>_lib — its dir IS its bazel pkg).
    #
    #   system/<name>_tester/component.art `package system.<name>_tester` — the APP
    #     IMPORTS system.<name>.* and prototypes its node into a composition. gen-app
    #     --kind fc --out apps emits apps/<Comp>/main that LINKS //system/<name>/lib
    #     (the imported node is NOT regenerated — cross-package link, the payoff).
    #
    # They MUST be separate dirs: the loader auto-merges a package.art+component.art
    # PAIR in one dir as ONE package (their `package` lines must match), so two
    # different packages can't share a dir.
    _write(f"system/{name}/package.art", _sub(_PKG_PACKAGE_ART))
    _write(f"system/{name}_tester/component.art", _sub(_PKG_COMPONENT_ART))
    # The whole-tree parse entry: `package system` at system/system.art, importing
    # ONLY system.<name>_tester.* (which transitively pulls system.<name>). NO
    # supervisor import — the aggregate parse validates the package tree, not deploy.
    _write("system/system.art", _sub(_PKG_SYSTEM_ART))

    # The generic rig (one machine, this package's app). Reuse the WORKSPACE rig
    # templates verbatim: the dev/test app is `package system.apps` → gen-manifest
    # emits manifest/apps/manifest.py exactly as a workspace does, so the same
    # guarded-import rig (labels + process names come from the generated manifest,
    # NOT hand-written) works unchanged. With --with-services it also folds in the
    # framework's ARA services manifest.
    # (The package's OWN generated impl goes to packages/<name>/{lib,impl} via
    #  `gen-app --kind package … --out packages/<name>`, matching the //packages/
    #  <name> bazel label the composition derives from `import packages.<name>.*`.)
    _write("manifest/__init__.py", "")
    _write("manifest/rig/__init__.py", "")
    _write("manifest/rig/rig.py",
           (_INIT_RIG_PY_SERVICES if with_services else _INIT_RIG_PY)
           .replace("@NAME@", name))
    # test/ — the probe RF suite: binds a client identity and calls the node's
    # ProbeCtl over TIPC (the port connector; NO ProbeDaemon node in the exe).
    _write(f"test/{name}.robot", _sub(_PKG_TEST_ROBOT))
    _write(f"test/{name}_lib.py", _sub(_PKG_TEST_LIB))

    # Build wiring vs the sibling @pero_theia (same as a workspace).
    try:
        theia_rel = _os.path.relpath(theia_root, ws)
    except ValueError:
        theia_rel = str(theia_root)
    _write(".theia", f"THEIA_ROOT={theia_rel}\nname={name}\nkind=package\n")
    _write("MODULE.bazel", _INIT_MODULE_BAZEL.replace("@MODNAME@", slug)
                                             .replace("@THEIA_REL@", theia_rel))
    _write(".bazelrc", _INIT_BAZELRC)
    _sync_pin(".bazelversion", _read_or(theia_root / ".bazelversion", "9.1.0"))
    # The TESTER app's proto shims. In the two-package layout the runnable app is
    # system.@NAME@_tester (NOT system.apps), so its proto lands at
    # proto/system/@NAME@_tester/@NAME@_tester.proto. gen-app --kind fc writes that
    # .proto but NOT its nanopb genrule BUILD (fc-mode emits no proto BUILD), so the
    # scaffold supplies it here + the //proto:platform_protos aggregate that links
    # it. (The PACKAGE's own proto, system/@NAME@, gets a self-contained BUILD from
    # gen-app --kind package — no shim needed.) apps/__init__.py + deploy/ gitkeeps
    # + .mcp.json round out the workspace shape so dev-loop tooling works unchanged.
    _write("proto/BUILD.bazel", _sub(_PKG_PROTO_AGG))
    _write(f"proto/system/{name}_tester/BUILD.bazel", _sub(_PKG_TESTER_PROTO_BUILD))
    _write("apps/__init__.py", "")
    _write("deploy/registry/.gitkeep", "")
    _write("deploy/config/.gitkeep", "")
    _write(".mcp.json", _INIT_MCP_JSON.replace("@THEIA_ROOT@", str(theia_root)))
    _write("local_setup.sh", _INIT_SETUP_LOCAL.replace("@NAME@", name))
    if (ws / "local_setup.sh").exists():
        (ws / "local_setup.sh").chmod(0o755)
    # apps/ + proto/ generated trees are GITIGNORED (gen-app output, not source).
    _write(".gitignore", _sub(_PKG_GITIGNORE))
    _write("README.md", _sub(_PKG_README))

    flavour = "package (+services)" if with_services else "package"
    print(f"\ntheia init: scaffolded '{name}' {flavour} against {theia_root}",
          file=sys.stderr)
    for c in created:
        print(f"  + {c}", file=sys.stderr)
    print(_sub(_PKG_NEXT_STEPS), file=sys.stderr)
    return 0


_INIT_SYSTEM_ART = '''\
// @NAME@ — Theia consuming-workspace aggregator. `theia manifest` walks this file.
//
// It imports ONLY this workspace's own app package (system/@NAME@). The supervisor
// is a DEPLOY-time fabric — `theia manifest`/`install` stage it from the fixed
// framework target (//platform/supervisor/main:supervisor, $THEIA_ROOT), NOT from
// ART — so there is no system.supervisor import and no system/{platform,supervisor}
// symlink in a bare workspace.
//
// EMPTY-workspace shape: an empty Applications cluster. Add your app by declaring a
// `composition` in system/@NAME@/component.art and `cluster Applications { composition
// <Yours> <id> }` — it flows here via the import below, no edit needed.

package system

import system.@NAME@.*       // THIS workspace's app package (system/@NAME@, real dir)

cluster Applications { }     // empty until you add a composition in system/@NAME@/
'''

_INIT_APPS_PACKAGE_ART = '''\
// @NAME@ — messages + interfaces + node(s) for this workspace's application (the
// SWP named @NAME@). The package is `system.@NAME@`, sourced at system/@NAME@/
// (FQN ↔ dir 1:1). You EDIT this file: add your messages/interfaces/nodes.
//
// It ships ONE placeholder node (@CLS@Node) so the toolchain runs end to end on a
// fresh scaffold — parse → gen-app → build → install → start a real supervised
// node. Rename it, add ports/messages, or add more nodes as your app grows.

package system.@NAME@

message @CLS@Empty { }

// A tiny request/reply surface so the node has something to serve (and so a probe
// / `theia call` can poke it). Replace with your real interface.
interface clientServer @CLS@CtlIf {
    operation Ping(in p: @CLS@Empty) returns @CLS@Empty
}

// The app's node. tipc 0xD0010001 (pick your own range as you add nodes). A
// GenServer serving @CLS@CtlIf; the impl body is the write-once
// apps/@CLS@/impl/@CLS@Node_handlers.cc after gen-app.
node atomic @CLS@Node {
    tipc type=0xD0010001 instance=0
    reporting = true
    tag = "@CLS@"
    ports {
        server ctl provides @CLS@CtlIf
    }
}
'''

_INIT_APPS_COMPONENT_ART = '''\
// @NAME@ — composition + cluster wiring for this workspace's app (SWP @NAME@).
//
// It ships ONE composition (@CLS@) prototyping the placeholder @CLS@Node, and the
// `cluster Applications` member that deploys it — so `gen-app --kind fc … --out
// apps` emits apps/@CLS@/main (→ //apps/@CLS@/main), and manifest/install/start run
// a real supervised node on a fresh scaffold.
//
// To add another app: declare a node in package.art, then here:
//   composition MyApp   { prototype MyNode my_node }
//   cluster Applications { composition MyApp my_app }   (add the member)
// then re-run `artheia gen-app --kind fc system/@NAME@/component.art --out apps
// --proto-out proto` (--proto-out lands @NAME@.proto where proto/'s BUILD expects
// it) and `bazel build //apps/...` (compiles against @pero_theia).

package system.@NAME@

composition @CLS@ {
    prototype @CLS@Node @NAME@
}

cluster Applications {
    composition @CLS@ app
}
'''

_INIT_RIG_PY = '''\
"""@NAME@ BOOTSTRAP rig — one machine ("central") running this workspace's apps.

The smoke-test target for a FRESH workspace: it lets `theia manifest @NAME@
&& theia install && theia start` run before you have any real deploy targets,
so you can verify the toolchain end to end on a clean scaffold. Addressed as
`bootstrap` because it lives at manifest/@NAME@/rig.py (manifest.<target>.rig);
the real per-target rigs (manifest/single/rig.py, …) come later and sit beside
it, never replacing it.

A :class:`DeploymentLayer` on the orthogonal-ARA engine
(:mod:`artheia.manifest.deployment`). It combines the workspace's generated
apps manifest (the BASE — open machines) with a deploy delta: one machine and
every process bound to it. `theia manifest @NAME@` reads the RIG export.

The apps manifest is gen-manifest output. Until you run it the import fails, so
it is guarded — a fresh workspace resolves to an EMPTY deployment (one machine,
no processes), which is enough to verify the toolchain. As you add compositions
to system/@NAME@/component.art and regenerate (`artheia gen-manifest
system/@NAME@/component.art manifest/@NAME@/manifest.py`), the processes +
applications flow in automatically.

See $THEIA_ROOT/docs/skills/theia/references/deployment.md.
"""
from __future__ import annotations

from artheia.manifest.algebra import Append, Explicit
from artheia.manifest.deployment import (
    ApplicationLayer,
    ApplicationSetLayer,
    DeploymentLayer,
    ExecutionLayer,
    MachineLayer,
    MachineSetLayer,
    ProcessLayer,
)

# The generated apps manifest (a base DeploymentLayer with machines left open).
# Not present until the first `gen-manifest` — guard the import so a fresh
# workspace still imports + serializes (an empty deployment).
try:
    from manifest.@NAME@.manifest import DEPLOYMENT as _APPS
except Exception:               # not generated yet → empty workspace
    _APPS = DeploymentLayer()

# Every process + application the apps base declares (bind each to the one
# machine below). The base's application(s) come from gen-manifest, named after
# the composition's package (e.g. `sanity_tester`), NOT a fixed "apps" — so bind
# host_machine onto EACH by name rather than appending a hardcoded app (which
# would leave the real one host-less and add an empty duplicate).
from artheia.manifest.deployment import _members as _set_members
_PROCESS_NAMES = sorted(p.name for p in _set_members(_APPS.execution.processes))
_APP_NAMES = sorted(a.name for a in _set_members(_APPS.applications.applications))

# The deploy delta: one machine "central"; every app process + application bound
# to it. Combined onto the apps base. If the base declares no application yet
# (empty workspace), fall back to a single "apps" AA so the bootstrap still runs.
RIG = _APPS.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={
        MachineLayer(name="central", arch=Explicit("x86_64"),
                     cores={0, 1, 2, 3}, machine_states={"Startup", "Running"}),
    }),
    execution=ExecutionLayer(processes={
        Append(ProcessLayer(name=n, machine=Explicit("central")))
        for n in _PROCESS_NAMES
    }),
    applications=ApplicationSetLayer(applications={
        Append(ApplicationLayer(name=a, host_machine=Explicit("central")))
        for a in (_APP_NAMES or ["apps"])
    }),
))

# Optional supervisor sidecar (gen-manifest writes manifest/@NAME@/executor.py).
# serialize-manifest reads SUPERVISORS off this module if present.
try:
    from manifest.@NAME@.executor import SUPERVISORS
except Exception:
    SUPERVISORS = []

# Per-process node/module metadata for the executor.json worker leaves.
# gen-manifest emits PROCESS_NODES onto manifest.@NAME@.manifest.
try:
    from manifest.@NAME@.manifest import PROCESS_NODES
except Exception:
    PROCESS_NODES = {}

# Static params defaults (params{} declared in .art) for config/<fc>.json.
try:
    from manifest.@NAME@.manifest import PROCESS_PARAMS
except Exception:
    PROCESS_PARAMS = {}

# Etcd config-defaults (config{} declared values + digest) for first-boot seed.
try:
    from manifest.@NAME@.manifest import PROCESS_CONFIG_DEFAULTS
except Exception:
    PROCESS_CONFIG_DEFAULTS = {}
'''

# --- --with-services variants ---------------------------------------------

_INIT_SYSTEM_ART_SERVICES = '''\
// @NAME@ — Theia consuming-workspace aggregator (WITH the ARA services).
//
// Imports the platform services (com / log / per / sm / ucm / shwa) + the
// supervisor + THIS workspace's app package. `theia start` brings the full
// service tree up under the supervisor; add your apps under apps/.

package system

import system.services.*     // the ARA platform FCs (system/services → framework)
import system.supervisor.*   // the OTP-style supervisor (the runtime fabric)
import system.@NAME@.*       // THIS workspace's app package (system/@NAME@, real dir)

// --- forward-decl: the clusters this workspace deploys --------------------
cluster Services     { }     // the platform FCs (materialized from the import)
cluster Applications { }     // empty until you add a composition in apps/

composition Supervisor { }

// --- this workspace's deployment ------------------------------------------
cluster Platform {
    composition Supervisor  sup
}
'''

_INIT_RIG_PY_SERVICES = '''\
"""@NAME@ BOOTSTRAP rig — one machine: the ARA services + this workspace's apps.

The smoke-test target for a fresh WITH-SERVICES workspace, addressed as
`bootstrap` (manifest/@NAME@/rig.py): `theia manifest @NAME@ && theia
install && theia start` verifies the toolchain before real deploy targets exist.

A :class:`DeploymentLayer` (orthogonal-ARA engine) built by combining the
framework's services manifest (the full FC set: com/log/per/sm/ucm/shwa…) with
this workspace's generated apps manifest, then a deploy delta binding everything
to one machine ("central"). `theia install` builds the FC binaries + the
supervisor; `theia start` runs the whole service tree with your apps under it.

As you add compositions to system/@NAME@/component.art and regenerate
(`artheia gen-manifest system/@NAME@/component.art manifest/@NAME@/manifest.py`),
the app processes + applications flow in automatically.

See $THEIA_ROOT/docs/skills/theia/references/deployment.md.
"""
from __future__ import annotations

import importlib.util as _ilu
import os as _os
import sys as _sys

from artheia.manifest.algebra import Append, Explicit
from artheia.manifest.deployment import (
    ApplicationLayer,
    ApplicationSetLayer,
    DeploymentLayer,
    ExecutionLayer,
    MachineLayer,
    MachineSetLayer,
    ProcessLayer,
    _members as _set_members,
)


def _load_services_manifest(mod):
    """Load the framework's services manifest module (``manifest.py`` /
    ``executor.py``) by PATH from $THEIA_ROOT/manifest/services/ — NOT via a
    generic top-level ``manifest`` package on PYTHONPATH. The framework ships
    this tree at /opt/theia/manifest (or the source checkout's manifest/), so
    nothing pollutes the user's global import namespace. Returns the module, or
    None if the framework has no services manifest (a bare, no-services rig)."""
    root = _os.environ.get("THEIA_ROOT")
    if not root:
        return None
    path = _os.path.join(root, "manifest", "services", mod + ".py")
    if not _os.path.isfile(path):
        return None
    name = "_theia_services_" + mod
    spec = _ilu.spec_from_file_location(name, path)
    m = _ilu.module_from_spec(spec)
    _sys.modules[name] = m
    spec.loader.exec_module(m)
    return m


_svc_manifest = _load_services_manifest("manifest")
_svc_executor = _load_services_manifest("executor")

# The framework's ARA services manifest (a base DeploymentLayer, machines open).
_SERVICES = _svc_manifest.DEPLOYMENT if _svc_manifest else DeploymentLayer()

# This workspace's generated apps manifest (empty until you add apps). Guarded:
# a fresh workspace has no manifest/@NAME@/manifest.py yet → services-only.
# manifest.@NAME@ is the WORKSPACE's own package (this dir is on sys.path when the
# rig runs), so a plain import is correct — no global-namespace concern.
try:
    from manifest.@NAME@.manifest import DEPLOYMENT as _APPS
except Exception:               # not generated yet → services-only
    _APPS = DeploymentLayer()

# The assembled base: services ⊕ apps (machines still open).
_BASE = _SERVICES.combine(_APPS)
_PROCESS_NAMES = sorted(p.name for p in _set_members(_BASE.execution.processes))
# Every application the base declares (platform "services" AA + the app AA, which
# gen-manifest names after the composition's package, e.g. `sanity_tester`). Bind
# host_machine onto EACH by name — don't hardcode "apps" (would leave the real
# app AA host-less). "services" is always present (from the framework manifest).
_APP_NAMES = sorted(a.name for a in _set_members(_BASE.applications.applications))

# The deploy delta: one machine "central"; every process + application bound to it.
RIG = _BASE.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={
        MachineLayer(name="central", arch=Explicit("x86_64"),
                     cores={0, 1, 2, 3}, machine_states={"Startup", "Running"}),
    }),
    execution=ExecutionLayer(processes={
        Append(ProcessLayer(name=n, machine=Explicit("central")))
        for n in _PROCESS_NAMES
    }),
    applications=ApplicationSetLayer(applications={
        Append(ApplicationLayer(name=a, host_machine=Explicit("central")))
        for a in (_APP_NAMES or ["services", "apps"])
    }),
))

# Merged supervisor sidecar (services + apps executor trees under one root).
# Read by serialize-manifest off this module if present.
try:
    from artheia.manifest.supervisor import RestartStrategy, SupervisorNode
    _SVC_SUP = _svc_executor.SUPERVISORS if _svc_executor else []
    try:
        from manifest.@NAME@.executor import SUPERVISORS as _APP_SUP
    except Exception:
        _APP_SUP = []
    _SUBTREES = [n for n in (_SVC_SUP + _APP_SUP) if n.name != "root"]
    SUPERVISORS = [
        SupervisorNode(name="root", strategy=RestartStrategy.ONE_FOR_ALL,
                       children=[n.name for n in _SUBTREES]),
        *_SUBTREES,
    ]
except Exception:
    SUPERVISORS = []

# Merged per-process node/module metadata (services ⊕ apps) for the
# executor.json worker leaves.
try:
    _SVC_NODES = _svc_manifest.PROCESS_NODES if _svc_manifest else {}
    try:
        from manifest.@NAME@.manifest import PROCESS_NODES as _APP_NODES
    except Exception:
        _APP_NODES = {}
    PROCESS_NODES = {**_SVC_NODES, **_APP_NODES}
except Exception:
    PROCESS_NODES = {}

# Static params defaults (services ⊕ apps) for config/<fc>.json.
try:
    _SVC_PARAMS = _svc_manifest.PROCESS_PARAMS if _svc_manifest else {}
    try:
        from manifest.@NAME@.manifest import PROCESS_PARAMS as _APP_PARAMS
    except Exception:
        _APP_PARAMS = {}
    PROCESS_PARAMS = {**_SVC_PARAMS, **_APP_PARAMS}
except Exception:
    PROCESS_PARAMS = {}

# Etcd config-defaults (services ⊕ apps) for first-boot seed.
try:
    _SVC_CD = _svc_manifest.PROCESS_CONFIG_DEFAULTS if _svc_manifest else {}
    try:
        from manifest.@NAME@.manifest import PROCESS_CONFIG_DEFAULTS as _APP_CD
    except Exception:
        _APP_CD = {}
    PROCESS_CONFIG_DEFAULTS = {**_SVC_CD, **_APP_CD}
except Exception:
    PROCESS_CONFIG_DEFAULTS = {}
'''

# setup_local.sh — the per-workspace activation shim (theia init writes it).
#
# Source THIS from anywhere (`source /path/to/@NAME@/setup_local.sh`) — it knows
# its OWN location (the workspace root), so THEIA_WORKSPACE is always THIS dir,
# not $PWD-at-source-time (the fragile part of sourcing env.sh directly). It:
#   1. resolves its own dir   → THEIA_WORKSPACE
#   2. finds the framework    → from .theia's THEIA_ROOT (source checkout) or
#                               /opt/theia (the installed deb)
#   3. sources it             → $THEIA_ROOT/env.sh | /opt/theia/setup.sh
#   4. RE-pins THEIA_WORKSPACE → this dir (the framework activation may have set
#                               it to $PWD; we override with the authoritative one)
# Idempotent + relocatable: re-sourcing is safe; moving the ws+theia pair keeps
# working (THEIA_ROOT in .theia is relative when they share a prefix).
# .mcp.json — MCP servers for a Claude Code session in THIS consuming workspace.
# @THEIA_ROOT@ is the installed framework prefix (/opt/theia from the deb, or a
# source checkout). artheia + rf-theia run from the workspace .venv (pip-installed
# there); the theia dev-loop server + work-with-me ship under $THEIA_ROOT. Each
# server is launched with cwd = this workspace (Claude Code runs the command from
# the .mcp.json dir), so `.venv/bin/python` + THEIA_INVOCATION_CWD resolve here.
_INIT_MCP_JSON = '''\
{
  "mcpServers": {
    "artheia": {
      "type": "stdio",
      "command": ".venv/bin/python",
      "args": ["-m", "artheia.adapters.mcp_server"]
    },
    "theia": {
      "type": "stdio",
      "command": ".venv/bin/python",
      "args": ["@THEIA_ROOT@/tools/theia_mcp.py"],
      "env": { "PYTHONPATH": "@THEIA_ROOT@/tools" }
    },
    "rf-theia": {
      "type": "stdio",
      "command": ".venv/bin/python",
      "args": ["-m", "rf_theia.adapters.mcp_server"]
    },
    "work-with-me": {
      "type": "stdio",
      "command": ".venv/bin/python",
      "args": ["@THEIA_ROOT@/skills/work-with-me/server.py"]
    }
  }
}
'''


_INIT_SETUP_LOCAL = '''\
#!/bin/sh
# AUTO-GENERATED by `theia init` for the @NAME@ workspace. Source from anywhere:
#   source /path/to/@NAME@/setup_local.sh
# It activates the framework AND pins THEIA_WORKSPACE to this dir.

# 1. This script's dir = the workspace root (works under bash + zsh + sh).
if [ -n "${ZSH_VERSION:-}" ]; then _src="${(%):-%x}"; else _src="${BASH_SOURCE[0]:-$0}"; fi
_THEIA_WS="$(cd "$(dirname "$_src")" && pwd)"

# 2. Find the framework: .theia records THEIA_ROOT (relative to the ws, or abs);
#    fall back to the installed deb prefix.
_THEIA_ROOT=""
if [ -f "$_THEIA_WS/.theia" ]; then
    _rec="$(sed -n 's/^THEIA_ROOT=//p' "$_THEIA_WS/.theia" | head -1)"
    case "$_rec" in
        /*) _THEIA_ROOT="$_rec" ;;                       # absolute
        ?*) _THEIA_ROOT="$(cd "$_THEIA_WS/$_rec" 2>/dev/null && pwd)" ;;  # relative to ws
    esac
fi
[ -z "$_THEIA_ROOT" ] && [ -d /opt/theia ] && _THEIA_ROOT=/opt/theia

# 3. Source the framework activation (env.sh for a source checkout; setup.sh for
#    the deb). Quietly skip if neither is present (caller sees the unset vars).
if [ -f "$_THEIA_ROOT/env.sh" ]; then
    . "$_THEIA_ROOT/env.sh"
elif [ -f "$_THEIA_ROOT/setup.sh" ]; then
    . "$_THEIA_ROOT/setup.sh"
else
    echo "setup_local.sh: framework not found (THEIA_ROOT='$_THEIA_ROOT') — " \\
         "set it in .theia or install /opt/theia" >&2
fi

# 4. Authoritative THEIA_WORKSPACE = this dir (override whatever step 3 set from
#    $PWD). Deploy tooling reads $THEIA_WORKSPACE/deploy/{registry,config}/.
export THEIA_WORKSPACE="$_THEIA_WS"
'''


_INIT_README = '''\
# @NAME@ — Theia consuming workspace

Built against a SIBLING Theia source checkout (THEIA_ROOT=@THEIA_ROOT@),
not vendored. A bare workspace plants NO framework symlinks — system.art imports
only system.apps.*, and the supervisor is staged from $THEIA_ROOT at manifest/
install time. Only `--with-services` symlinks the framework .art it imports
(system/{platform/runtime,supervisor,services}).

```sh
source @THEIA_ROOT@/env.sh       # activate the sibling framework checkout:
                                 # THEIA_ROOT + its .venv + `theia`/`tdb` on PATH,
                                 # THEIA_WORKSPACE=this dir
theia init                       # (already run — scaffolded this dir)
# link your app/gateway spec, import it in system/system.art, then:
theia manifest
theia install
theia start && tdb ps
```

When Theia ships as a deb, swap the sibling source checkout for the installed
prefix: `source /opt/theia/setup.sh` (THEIA_ROOT=/opt/theia) — the symlinks +
rig stay the same. (env.sh is the source-checkout activation; setup.sh ships
only inside the deb.)
'''

# ── Bazel-module scaffold (the workspace builds its OWN app C++) ────────────

_INIT_MODULE_BAZEL = '''\
# @MODNAME@ — a Theia consuming workspace's Bazel module. Builds this workspace's
# OWN app C++ (apps/<App>/...) against the sibling Theia source tree, consumed as
# the `pero_theia` module via local_path_override (the gataway_ws pattern). The
# gen-app BUILD files reference //platform/runtime, //proto,
# //platform/supervisor/tombstone — resolved by the alias shims under platform/
# that forward to @pero_theia//... (the app's OWN proto under proto/system/apps
# builds locally).
module(name = "@MODNAME@", version = "0.1.0")

bazel_dep(name = "platforms", version = "0.0.11")
bazel_dep(name = "rules_cc", version = "0.2.17")
bazel_dep(name = "rules_pkg", version = "1.1.0")
bazel_dep(name = "rules_python", version = "1.7.0")
bazel_dep(name = "nanopb", version = "0.4.9.1")

# The Theia source tree — the runtime/supervisor/proto labels the apps compile
# against. Sibling checkout ($THEIA_ROOT); swap for /opt/theia once Theia is a deb.
bazel_dep(name = "pero_theia", version = "0.1.0")
local_path_override(module_name = "pero_theia", path = "@THEIA_REL@")

# Hermetic proto codegen (@theia_codegen) — the content-hashed protoc + nanopb a
# PACKAGE's proto genrule declares as `tools` (proto/packages/<X>/BUILD.bazel from
# gen-app --kind package). Re-exported from @pero_theia's toolchains/codegen so a
# consuming workspace resolves @theia_codegen the same way the framework does.
# (Workspace-only apps under proto/system/apps/ use the host nanopb and don't need
# it, but declaring it is harmless — only referenced when a package proto builds.)
theia_codegen_ext = use_extension(
    "@pero_theia//toolchains/codegen:extension.bzl", "theia_codegen_ext")
use_repo(theia_codegen_ext, "theia_codegen")
'''

_INIT_BAZELRC = '''\
# Consuming-workspace Bazel config (mirrors the framework, host-only).
build --enable_bzlmod
build --incompatible_enable_cc_toolchain_resolution
build --action_env=PATH
# OPTIMIZED by default. Bazel's stock `fastbuild` is -O0 — unsafe for real-time
# nodes (observed: a 20 Hz feed vs ~4 Hz -O0 planning → the brake landed late).
# Keep frame pointers so libtombstone backtraces stay walkable at -O2. Debug
# builds are explicit: `bazel build -c dbg //...`.
build -c opt
build --copt=-fno-omit-frame-pointer
# nanopb pb.h location varies by libnanopb-dev version (flat /usr/include vs
# /usr/include/nanopb/) — add the subdir so the generated *.pb.h #include <pb.h>
# resolves either way (harmless when absent).
build --copt=-I/usr/include/nanopb
build:linux --cpu=k8
build:linux --compiler=gcc
build --config=linux
'''

_INIT_PROTO_AGG = '''\
# //proto:platform_protos — the nanopb wire types the gen-app lib links. This
# workspace builds its OWN app proto (system/@NAME@, nanopb-compiled below) + pulls
# the runtime control proto from @pero_theia. (The framework aggregates all the
# FC protos under //platform/proto; a consuming workspace only needs its own app
# proto + the runtime one — the lib #includes "system/@NAME@/@NAME@.pb.h". The app
# proto lives under proto/, never mixed with the framework's platform/proto/.)
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "platform_protos",
    srcs = ["//proto/system/@NAME@:@NAME@_pb_c"],
    hdrs = ["//proto/system/@NAME@:@NAME@_pb_h"],
    includes = ["."],   # callers #include "system/@NAME@/@NAME@.pb.h"
    copts = ["-fPIC"],
    deps = ["@pero_theia//platform/runtime:runtime_proto_cc"],
)
'''

_INIT_APPS_PROTO_BUILD = '''\
# nanopb sources for the system.@NAME@ package. gen-app (--proto-out proto)
# writes @NAME@.proto AND @NAME@.options here (it auto-pins every string/bytes field
# to a fixed char[]; override per field with an .art `[max_size:N]`). Both feed
# this genrule (.options auto-loaded by nanopb). .pb.{c,h} are BUILT, not committed.
package(default_visibility = ["//visibility:public"])

genrule(
    name = "@NAME@_pb",
    srcs = ["@NAME@.proto"] + glob(["@NAME@.options"], allow_empty = True),
    outs = ["@NAME@.pb.c", "@NAME@.pb.h"],
    cmd = "set -e;"
        + " in_dir=$$(dirname $(location @NAME@.proto));"
        + " out_dir=$$(dirname $(location @NAME@.pb.c));"
        + " nanopb_generator -I $$in_dir -D $$out_dir @NAME@.proto;",
    local = True,
)
filegroup(name = "@NAME@_pb_c", srcs = ["@NAME@.pb.c"])
filegroup(name = "@NAME@_pb_h", srcs = ["@NAME@.pb.h"])
filegroup(name = "@NAME@_proto", srcs = ["@NAME@.proto"])
'''

# ── package-flavour proto shims ─────────────────────────────────────────────
# A PACKAGE workspace has TWO proto trees (mirroring its two .art packages):
#   proto/system/@NAME@/          — the package's own messages. gen-app --kind
#                                   package emits a SELF-CONTAINED BUILD there
#                                   (//proto/system/@NAME@:@NAME@_proto), so no
#                                   scaffold shim is needed for it.
#   proto/system/@NAME@_tester/   — the tester app's messages (its no-arg-op
#                                   request wrappers etc.). gen-app --kind fc
#                                   writes ONLY the .proto here (fc-mode emits no
#                                   proto BUILD), so the scaffold MUST provide the
#                                   nanopb genrule BUILD, exactly as a ws does for
#                                   system/apps. The aggregate //proto:platform_protos
#                                   the tester lib links points at THIS tree.
_PKG_PROTO_AGG = '''\
# //proto:platform_protos — the nanopb wire types the TESTER app lib links. The
# tester (system.@NAME@_tester) #includes "system/@NAME@_tester/@NAME@_tester.pb.h";
# this aggregate nanopb-compiles that proto (genrule below) + pulls the runtime
# control proto from @pero_theia. The PACKAGE's own proto (system/@NAME@) is a
# SEPARATE, self-contained target (//proto/system/@NAME@:@NAME@_proto) the tester
# lib links directly — not folded in here. Proto lives under proto/, never mixed
# with the framework's platform/proto/.
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "platform_protos",
    srcs = ["//proto/system/@NAME@_tester:@NAME@_tester_pb_c"],
    hdrs = ["//proto/system/@NAME@_tester:@NAME@_tester_pb_h"],
    includes = ["."],   # callers #include "system/@NAME@_tester/@NAME@_tester.pb.h"
    copts = ["-fPIC"],
    deps = ["@pero_theia//platform/runtime:runtime_proto_cc"],
)
'''

_PKG_TESTER_PROTO_BUILD = '''\
# nanopb sources for the system.@NAME@_tester package (the tester app). gen-app
# --kind fc (--proto-out proto) writes @NAME@_tester.proto AND @NAME@_tester.options
# here (auto-pins every string/bytes field to a fixed char[]; override per field
# with an .art `[max_size:N]`). Both feed this genrule (.options auto-loaded by
# nanopb). .pb.{c,h} are BUILT, not committed. fc-mode emits no proto BUILD, so
# this scaffold shim supplies it (the aggregate //proto:platform_protos links it).
package(default_visibility = ["//visibility:public"])

genrule(
    name = "@NAME@_tester_pb",
    srcs = ["@NAME@_tester.proto"] + glob(["@NAME@_tester.options"], allow_empty = True),
    outs = ["@NAME@_tester.pb.c", "@NAME@_tester.pb.h"],
    cmd = "set -e;"
        + " in_dir=$$(dirname $(location @NAME@_tester.proto));"
        + " out_dir=$$(dirname $(location @NAME@_tester.pb.c));"
        + " nanopb_generator -I $$in_dir -D $$out_dir @NAME@_tester.proto;",
    local = True,
)
filegroup(name = "@NAME@_tester_pb_c", srcs = ["@NAME@_tester.pb.c"])
filegroup(name = "@NAME@_tester_pb_h", srcs = ["@NAME@_tester.pb.h"])
filegroup(name = "@NAME@_tester_proto", srcs = ["@NAME@_tester.proto"])
'''


# ── theia init --kind package templates ─────────────────────────────────────
# A generic package: ONE real node (@CLS@Node) with a ProbeCtl the test drives,
# ONE composition that starts it, a forward-decl aggregator, a generic rig, and
# an RF probe test. @NAME@=name, @SLUG@=py-ident, @CLS@=CamelCase.

_PKG_PACKAGE_ART = '''\
// @NAME@ — a Theia PACKAGE: messages + interfaces + node(s), ONE linkable unit.
// `package system.@NAME@` — the FQN mirrors the dir (system/@NAME@/) 1:1, no
// tautological `packages/` prefix. Built ONCE as a lib via
//   artheia gen-app --kind package system/@NAME@/package.art --out src
// → src/{lib,impl} → //src/lib:@NAME@_lib. A composition (the tester below, or a
// real user workspace) IMPORTS system.@NAME@ and links that prebuilt lib.
//
// Edit this: add your messages, interfaces, and node ports. Multiple nodes are
// fine — they build as ONE package lib. The ProbeCtl below is the test surface:
// the RF probe (test/@NAME@.robot) calls Ping over TIPC to prove the node serves.

package system.@NAME@


message @CLS@Empty { }

message @CLS@Status {
    uint32 seq   = 1
    bool   ready = 2
}

// The test/probe surface: the RF suite binds a client and calls these over TIPC
// (the "node impersonated by a probe" — a port connector, no ProbeDaemon node in
// the executable). Ping proves the node is up + serving in isolation.
interface clientServer CtlIf {
    operation Ping(in p: @CLS@Empty) returns @CLS@Status
}

// The package's real node. tipc 0x800100F0 (pick your own range). A GenServer
// serving CtlIf. Add real ports (senderReceiver streams, more ops) as your
// package grows; the impl handler lives in src/impl/@CLS@Ctrl_handlers.cc.
node atomic @CLS@Ctrl {
    tipc type=0x800100F0 instance=0
    reporting = true
    tag = "@CLS@"
    ports {
        server ctl provides CtlIf
    }
}
'''

_PKG_COMPONENT_ART = '''\
// @NAME@_tester — the dev/test app that assembles this package's node(s) into a
// runnable, in ISOLATION. It is a SEPARATE package (`package system.@NAME@_tester`,
// at system/@NAME@_tester/) that IMPORTS system.@NAME@ and prototypes its node —
// exactly how an EXTERNAL workspace consumes the package: the composition LINKS
// the prebuilt //src/lib:@NAME@_lib (the imported node is NOT regenerated). This
// in-repo tester proves the import/link path without a second repo.
//
// gen-app --kind fc on THIS file emits apps/@CLS@Tester/main (gitignored) that
// `theia install` stages + `theia start` runs; gen-manifest reads the cluster.
//
// ONE composition = one process. The RF probe (test/@NAME@.robot) is the test
// node — a port connector calling @CLS@Ctrl's CtlIf over TIPC, NOT a second
// composition and NOT a ProbeDaemon baked into the executable.

package system.@NAME@_tester

import system.@NAME@.*

composition @CLS@Tester {
    prototype @CLS@Ctrl @SLUG@
}

cluster @CLS@TesterRig {
    composition @CLS@Tester app
}
'''

_PKG_SYSTEM_ART = '''\
// @NAME@ — workspace aggregator: `artheia parse system/system.art` validates the
// whole package tree. `package system`, importing ONLY system.@NAME@_tester.*
// (which transitively pulls system.@NAME@). NO supervisor/platform import — the
// supervisor is a DEPLOY-time fabric resolved from $THEIA_ROOT by `theia
// manifest`, never referenced from ART. The tester's cluster (@CLS@TesterRig) is
// forward-declared here so the aggregate parse resolves before the C++ exists;
// its real body is in system/@NAME@_tester/component.art.

package system

import system.@NAME@_tester.*

extern cluster @CLS@TesterRig { }
'''


_PKG_TEST_ROBOT = '''\
*** Settings ***
Documentation     @NAME@ package probe — drives the live @CLS@Ctrl in isolation.
...
...               Runs the canonical Python probe (test/@NAME@_lib.py) which binds
...               a client identity via artheia.probe and calls @CLS@Ctrl's CtlIf
...               over TIPC (the "node impersonated by a probe" port connector). No
...               ProbeDaemon node in the executable — the probe IS the test agent.
...
...               Needs the node up (`theia start`); SKIPS cleanly otherwise.
Library           ${CURDIR}/@NAME@_lib.py

Force Tags        package-@NAME@    live    probe


*** Test Cases ***
@CLS@Ctrl Serves CtlIf Over TIPC
    [Documentation]    Ping the live node over TIPC; assert the call round-trips
    ...                (proves the cross-package link + wire). The node's `ready`
    ...                semantic is yours to implement in the handler.
    Require @CLS@Ctrl Listening
    Run @CLS@ Probe
'''

_PKG_TEST_LIB = '''\
"""Robot library: drive the @NAME@ package's @CLS@Ctrl via artheia.probe.

The probe binds a CLIENT identity and calls @CLS@Ctrl's CtlIf (Ping) over real
TIPC — the port connector that impersonates a peer, so the node is tested in
isolation with no ProbeDaemon node in the executable. Skips when the node isn't
bound (hermetic lane).
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

from robot.api import logger
from robot.api.deco import keyword, library

# test/@NAME@_lib.py → package repo root.
_WS = Path(__file__).resolve().parents[1]
# @CLS@Ctrl's TIPC service type (system.@NAME@ @CLS@Ctrl = 0x800100F0).
_NODE_TIPC_DEC = 0x800100F0


@library(scope="SUITE")
class @CLS@ProbeLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    @keyword("Require @CLS@Ctrl Listening")
    def require_listening(self) -> None:
        import subprocess
        try:
            out = subprocess.run(["tipc", "nametable", "show"],
                                 capture_output=True, text=True, timeout=10).stdout
        except (FileNotFoundError, subprocess.SubprocessError):
            logger.info("`tipc` unavailable — deferring to the probe's own gate.")
            return
        if str(_NODE_TIPC_DEC) not in out:
            from robot.api import SkipExecution
            raise SkipExecution(
                "@CLS@Ctrl not bound (TIPC %s absent) — `theia start` first."
                % hex(_NODE_TIPC_DEC))

    @keyword("Run @CLS@ Probe")
    def run_probe(self) -> None:
        sys.path.insert(0, str(Path(_WS).parent))   # allow artheia on the venv
        sys.path.insert(0, str(_WS / ".." / "artheia"))
        from artheia.gen_server.probe import ArtheiaContext
        # Parse the PACKAGE .art (system/@NAME@/package.art) — @CLS@Ctrl is a real
        # NodeDecl there (tipc addr + CtlIf ports). The tester component.art only
        # PROTOTYPES it, so it has no NodeDecl for the probe's RemoteRef namespace.
        art = _WS / "system" / "@NAME@" / "package.art"
        proto = _WS / "proto"
        ctx = ArtheiaContext(str(art), proto_root=str(proto))
        # CRITICAL: bind the probe's OWN source at a UNIQUE instance, NOT @CLS@Ctrl's
        # declared instance (0). Without the override the probe IMPERSONATES the node
        # — it binds the SAME TIPC address the live @CLS@Ctrl mux listens on; TIPC
        # anycast then routes the probe's own connect onto ITS OWN socket, not the
        # node's mux, and the call times out. A pid-derived source instance (the
        # tdb_client trick) gives the probe a distinct address; replies route by TYPE.
        probe_inst = (os.getpid() & 0x7FFF) | 0x8000
        probe = ctx.probe("@CLS@Ctrl", instance=probe_inst).start()
        try:
            rep = probe.call("@CLS@Ctrl", "Ping")
            # REACHABILITY is what this test proves: a well-formed reply means the
            # probe's connect reached @CLS@Ctrl's mux over TIPC, the op dispatched,
            # and the reply routed back — i.e. the whole cross-package link + wire
            # works. A None reply = the call never round-tripped (the real failure).
            if rep is None:
                raise AssertionError(
                    "@CLS@Ctrl did not answer Ping — no reply round-tripped "
                    "(is the node bound + the mux pumping?)")
            # `ready` is the node's OWN semantic — the fresh scaffold's write-once
            # handler (src/impl/@CLS@Ctrl_handlers.cc) returns a zero @CLS@Status
            # (ready=false) until you implement it. So log it as a HINT, don't fail
            # the reachability test on the stub. Once you set ready=true in the
            # handler, flip this to an assertion for a real readiness gate.
            ready = getattr(rep, "ready", rep.get("ready") if isinstance(rep, dict)
                            else None)
            logger.info("@CLS@Ctrl Ping round-tripped; ready=%s "
                        "(implement handle_call to set it true)" % ready)
        finally:
            probe.stop()
'''

_PKG_GITIGNORE = '''\
# GENERATED trees (gen-app / gen-manifest / install output — never committed).
# The scaffold's hand-written build shims + the WRITE-ONCE impl (handler bodies,
# state structs — user-owned after first emit) ARE committed, so they are
# re-included below and survive a fresh clone before any gen-app.
#
# The tester app tree — pure codegen, regenerated every gen-app --kind fc.
/apps/*
!/apps/__init__.py
# The PACKAGE's generated tree lives under src/. Its lib/ + BUILD.bazel are pure
# codegen (regen every gen-app --kind package). Its impl/ is WRITE-ONCE — the
# user owns the handler bodies + state structs + that BUILD after first emit — so
# src/impl/ is TRACKED (re-included) while src/lib/ is ignored.
/src/lib/
!/src/impl/
# Proto: the .pb.{c,h} are BUILT by the nanopb genrule (never committed). The
# .proto/.options are codegen too, but the hand-written shim BUILDs + the package's
# self-contained proto BUILD are re-included so a fresh clone builds.
/proto/*
!/proto/BUILD.bazel
!/proto/system/
/proto/system/@NAME@_tester/*
!/proto/system/@NAME@_tester/BUILD.bazel
/proto/system/@NAME@/*
!/proto/system/@NAME@/BUILD.bazel
/install/
/dist/
/manifest/@NAME@/manifest.py
/manifest/@NAME@/executor.py
/bazel-*
__pycache__/
*.pyc
'''

_PKG_README = '''\
# @NAME@ — a Theia package

A composable Theia PACKAGE (nodes + protocol + impl) a workspace consumes.

## Dev/test loop (runs unmodified after `theia init --kind package`)

This repo holds TWO .art packages, modelling the ROS import/link relationship:

- `system/@NAME@/package.art` (`package system.@NAME@`) — the package: messages +
  interfaces + node(s), built ONCE into a linkable lib under `src/`.
- `system/@NAME@_tester/component.art` (`package system.@NAME@_tester`) — a dev/test
  app that IMPORTS `system.@NAME@` and prototypes its node, exactly as an external
  workspace would. It LINKS the prebuilt `//src/lib:@NAME@_lib` (the imported node
  is NOT regenerated) — this in-repo tester proves the cross-package link.

```sh
source local_setup.sh
# 1. the PACKAGE (node/lib) — system/@NAME@ source → src/{lib,impl}:
artheia gen-app --kind package system/@NAME@/package.art --out src --proto-out proto --ns ara::@NAME@
# 2. the TESTER app — imports system.@NAME@, links //src/lib:@NAME@_lib (apps/ gitignored):
artheia gen-app --kind fc      system/@NAME@_tester/component.art --out apps --proto-out proto
# 3. manifest + install + run:
artheia gen-manifest system/@NAME@_tester/component.art manifest/@NAME@/manifest.py
theia manifest rig && theia install rig && theia start
# 4. probe test the node in isolation:
robot test/@NAME@.robot
```

## Consuming this package from another workspace

Clone this repo next to your workspace and depend on its bazel module; the
package resolves as `//packages/@NAME@/src/lib:@NAME@_lib`. In your app's
`.art`:  `import system.@NAME@.*`  then prototype `@CLS@Ctrl` in a composition —
gen-app links the prebuilt lib, it is never regenerated in your tree.
'''

_PKG_NEXT_STEPS = '''
Scaffold complete. The whole toolchain runs UNMODIFIED:
  source local_setup.sh
  # 1. the PACKAGE (node/lib) — system/@NAME@ source → src/{lib,impl}:
  artheia gen-app --kind package system/@NAME@/package.art --out src --proto-out proto --ns ara::@NAME@
  # 2. the TESTER app (imports system.@NAME@, links //src/lib:@NAME@_lib):
  artheia gen-app --kind fc      system/@NAME@_tester/component.art --out apps --proto-out proto
  artheia gen-manifest system/@NAME@_tester/component.art manifest/@NAME@/manifest.py
  theia manifest rig && theia install rig && theia start
  robot test/@NAME@.robot

Edit system/@NAME@/package.art (your nodes + protocol) +
src/impl/@CLS@Ctrl_handlers.cc (write-once, your bodies).
'''


COMMANDS = {
    "init":        (cmd_init,        "scaffold the CWD as a Theia consuming workspace (source or /opt/theia)"),
    "rig":         (cmd_rig,         "docker compose {up|down|status} the deploy stack (status: containers + live rtdb cluster)"),
    # provision/orchestrate/cleanup MOVED to the `colony` repo (deploy adapter).
    # theia emits the per-rig bundle via `manifest`/`dist`; colony deploys it.
    "install":     (cmd_install,     "build + populate install/<machine>/ (local host)"),
    "clean":       (cmd_clean,       "remove install/ + dist/manifest/; --bazel also runs bazel clean"),
    "stage-local": (cmd_install,     "alias for `install` (back-compat)"),
    "start":       (cmd_start,       "run the staged supervisor from install/<machine>/ (detached + pidfile)"),
    "stop":        (cmd_stop,        "stop the supervisor started by `theia start` (graceful)"),
    "cast":        (cmd_cast,        "cast <node> <msg> --data '{json}' [--instance N|--machine M] (test/demo)"),
    "call":        (cmd_call,        "call <node> <op> --data '{json}' [--instance N|--machine M] — prints reply (test/demo)"),
    "manifest":    (cmd_manifest,    "rig.py → dist/manifest/*.json (sole rig entry for deploy)"),
    "dist":        (cmd_dist,        "<target> [--arch A] — build debs from manifest (runtime deb-set or per-machine app bundle)"),
    "release":     (cmd_release,     "<target> [--s3 URL] — push runtime plane to S3; or (no target) build the full package set"),
    "release-swp": (cmd_release_swp, "build + publish a user-ws Software Package (day-2 Mender OTA, the package plane)"),
    "release-app": (cmd_release_swp, "alias for release-swp (deprecated)"),
    "release-role": (cmd_release_role, "build + publish a per-board role .mender (L4-C vehicle campaign)"),
    "cert":        (cmd_cert,        "{generate|copy} the SWP signing keypair; copy ships the PUBLIC verify key to colony via S3"),
    "compdb":      (cmd_compdb,      "regen compile_commands.json from bazel (clangd)"),
    "observer":    (cmd_observer,    "launch the supervisor-GUI against the local cluster (always mTLS)"),
}


def _usage() -> None:
    print("usage: theia <command> [args...]\n\ncommands:", file=sys.stderr)
    for name, (_fn, desc) in COMMANDS.items():
        print(f"  {name:<12} {desc}", file=sys.stderr)
    print("\n(.art / codegen lives on `artheia` — run `artheia --help`.)",
          file=sys.stderr)


def main(argv: list[str]) -> int:
    if not argv or argv[0] in ("-h", "--help"):
        _usage()
        return 0 if argv else 2
    # Hidden hook: emit the verb list (one per line) for shell completion, so
    # env.sh's completion stays in lockstep with COMMANDS instead of hardcoding
    # a list that drifts. No chdir / workspace needed.
    if argv[0] == "__complete":
        print("\n".join(COMMANDS))
        return 0
    name, rest = argv[0], argv[1:]
    entry = COMMANDS.get(name)
    if entry is None:
        print(f"theia: unknown command {name!r}\n", file=sys.stderr)
        _usage()
        return 2
    fn, _desc = entry
    # Preserve the caller's CWD before chdir — `init` scaffolds THERE, not in
    # the Theia checkout. Every other verb wants workspace-relative paths.
    os.environ.setdefault("THEIA_INVOCATION_CWD", str(Path.cwd()))
    os.chdir(WORKSPACE)  # bazel / compose / ansible paths are workspace-relative
    return fn(rest)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

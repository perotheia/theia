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
# provision/orchestrate/cleanup moved OUT to the `colony` repo (the deploy adapter):
# theia's deploy surface is now `manifest`/`dist` (emit the per-rig bundle to
# dist/manifest/<rig>); colony consumes that bundle + the deploy/registry. See
# docs/tasks/BACKLOG/repo-separation.md. (COMPOSE stays — the dev container stack.)


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
    # THEIA_ROOT itself on the path too: the manifest layer is a PEP-420
    # namespace package split across the framework (manifest/services) and the
    # workspace (manifest/apps) — a rig that imports both needs BOTH roots
    # visible so `manifest.services` (framework) and `manifest.apps` (workspace)
    # resolve into the one `manifest` namespace. Skipped when ws IS the framework.
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
    """docker compose up/down the deploy stack."""
    if not args or args[0] not in ("up", "down"):
        print("usage: theia rig {up|down} [compose-args...]", file=sys.stderr)
        return 2
    action, extra = args[0], list(args[1:])
    # `up` detaches by default so the shell returns; pass -d/--detach to keep.
    if action == "up" and not any(a in ("-d", "--detach") for a in extra):
        extra = ["-d", *extra]
    return _run(["docker", "compose", "-f", str(COMPOSE), action, *extra])



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


def _deb_mode() -> bool:
    """True when THEIA_ROOT is an INSTALLED prefix (the debs), not a source
    checkout — detected by the prebuilt supervisor under THEIA_ROOT/bin. In deb
    mode the framework binaries are already built (no bazel for //platform,
    //services); only the workspace's OWN app C++ is bazel-built."""
    return (THEIA_ROOT / "bin" / "supervisor").is_file()


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
    """The .art the gen-params emitter reads for an FC, derived from the bazel
    target. Services FCs (``//services/<fc>/...``) live at the canonical symlink
    path system/services/<fc>/package.art; app compositions (``//apps/...``) at
    system/apps/package.art. Returns a Path or None if absent (an FC with no
    .art simply gets no params file)."""
    if target.startswith("//services/"):
        cand = WORKSPACE / "system" / "services" / fc / "package.art"
    elif target.startswith("//apps/"):
        cand = WORKSPACE / "system" / "demo" / "package.art"
    else:
        return None      # platform FCs (gateway) carry their own params path
    return cand if cand.exists() else None


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

    log = dest / "supervisor.log"
    env = {
        **os.environ,
        "THEIA_SUPERVISOR_MANIFEST": "executor.json",
        "THEIA_ROOT_DIR": ".",
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
    # mTLS opt-in (mirrors deploy/run-supervisor.sh): when this machine's certs
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


def cmd_install(args: list[str]) -> int:
    """LOCAL install: build + populate $WORKSPACE/install/<machine>/.

        theia install [<target>] [--attr ATTR] [--machine M]

    The dev inner-loop counterpart of the remote .ipk deploy. <target> names a
    rig under manifest/<target>/rig.py (single / split / local; default
    ``single``) — the SAME target model as `theia manifest`. The deploy MANIFEST
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
    positionals = [a for a in args if not a.startswith("-")]
    target = positionals[0] if positionals else _DEFAULT_TARGET
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
        "--out", str(manifest_root),
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

    # 1c. Host/admin machine guard: a machine with NO processes (none in
    #     execution.json) is a host/admin node (console, no supervisor tree). For
    #     now stage just machines.json — no supervisor, no bin/, no executor.
    if not binaries:
        import json as _json
        dest.mkdir(parents=True, exist_ok=True)
        src_machines = manifest_root / "machines.json"
        if src_machines.is_file():
            shutil.copy2(src_machines, dest / "machines.json")
        print(f"theia install: '{machine}' has no processes (host/admin) — "
              f"staged {dest / 'machines.json'} only.", file=sys.stderr)
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

    # 3. executor.json — the supervisor tree for this machine. serialize-manifest
    #    ALREADY wrote install/manifest/<machine>/executor.json (step 1); copy it
    #    into install/<machine>/ where `theia start` reads it.
    dest.mkdir(parents=True, exist_ok=True)
    src_executor = manifest_root / machine / "executor.json"
    if not src_executor.is_file():
        print(f"theia install: no executor.json at {src_executor} — "
              "serialize-manifest did not emit the supervisor tree.",
              file=sys.stderr)
        return 1
    shutil.copy2(src_executor, dest / "executor.json")
    print(f"staged {dest / 'executor.json'}", file=sys.stderr)

    # 3b. Per-FC static params JSON — config/<fc>.json. Shared with the
    #     deploy-manifest path (theia manifest) so install/<machine>/config/ and
    #     dist/manifest/<machine>/config/ are produced by the SAME emitter.
    if (rc := _emit_fc_config(machine, binaries, dest / "config")) != 0:
        return rc

    # 4. Stage binaries + setcap. A binary's source is its prebuilt path (deb
    #    mode) when we have one, else its bazel-bin output.
    def _src(name: str, target: str) -> str:
        pb = prebuilt.get(target)
        return str(pb if pb is not None else _bazel_bin(target))
    bins = {n: _src(n, t) for n, t in binaries.items()}
    sup_src = _src("supervisor", supervisor_target)

    # Copy the binaries into install/<machine>/ + apply the setcap contract. This
    # is the SAME copy+caps a real deploy does — but a real deploy runs it over
    # SSH via Ansible (deploy/ansible/tasks/setcap.yml). Puppet is GONE; the
    # local stage is a plain Python copy + setcap (no engine to install).
    return _stage_local(dest, sup_src, bins)


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
                 binaries: dict[str, str]) -> int:
    """Copy the supervisor + child binaries into install/<machine>/ and apply
    the setcap contract. The local equivalent of the deploy's Ansible
    install-bundle + setcap (deploy/ansible/tasks/setcap.yml) — a plain Python
    copy + setcap, no orchestration engine. supervisor at <dest>/supervisor,
    children at <dest>/bin/<name>, all 0755; then setcap (bazel-out copies are
    read-only and a fresh copy clears caps, so setcap runs AFTER).

    Supervisor caps:
      cap_sys_nice   — realtime sched (SCHED_FIFO/RR) + CPU affinity on FC
                       node threads.
      cap_net_admin  — TIPC admin ops the supervisor performs: `tipc node set
                       clusterid` (rig isolation) + the topology-service
                       presence subscription com relies on. (Plain TIPC name
                       bind does NOT need it; cluster/topology admin does.)"""
    import shutil as _sh
    dest.mkdir(parents=True, exist_ok=True)
    # Release-dir layout (the ONLY layout — same shape a deployed rig + Mender use):
    # the supervisor binary sits at <dest>/supervisor (the updater, never swapped);
    # the CHILDREN live under a versioned release the `current` symlink points at,
    # so the supervisor launches each child's ./bin/<svc> from <dest>/current/bin/.
    # OTA flips current→releases/<v>; locally there's one release ("local"). The
    # supervisor's root_dir() REQUIRES <root>/current — no flat-bin fallback.
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

    def _copy(src: str, dst: Path) -> None:
        if dst.exists():
            # A prior copy may be read-only (bazel-out) or root-owned (a legacy
            # Puppet/sudo install). chmod+unlink as the user; on EPERM (root-
            # owned leftover), sudo rm it so the fresh user-owned copy lands.
            try:
                dst.chmod(0o755)
                dst.unlink()
            except PermissionError:
                _run(["sudo", "rm", "-f", str(dst)])
        _sh.copy2(src, dst)
        dst.chmod(0o755)
        print(f"  staged {dst}", file=sys.stderr)

    try:
        _copy(supervisor_src, dest / "supervisor")
        for name, src in binaries.items():
            _copy(src, rel / "bin" / name)   # children → releases/local/bin (via current/)
    except OSError as e:
        print(f"theia install: staging failed — {e}", file=sys.stderr)
        return 1

    # Supervisor caps (see docstring). Needs root; skip gracefully if
    # setcap/sudo unavailable (start still works, just without RT priority /
    # TIPC cluster isolation).
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
_DEFAULT_TARGET = "single"          # default test target (one-machine dev rig)
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
    "host":   {"cfg": "host",   "cpu": "x86_64",  "abi_key": "",
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
        labels.add(_qualify(_SUPERVISOR_TARGET))
        bins = "".join(f'\n        "{lbl}",' for lbl in sorted(labels))
        lines.append(f'dist_pkg(\n    name = "{h}",\n    binaries = [{bins}\n    ],\n)')
    (mdir / "BUILD.bazel").write_text("\n".join(lines) + "\n")


def cmd_manifest(args: list[str]) -> int:
    """Serialize a TEST TARGET's rig to the per-machine JSON manifest set.

        theia manifest [<target>] [--attr ATTR] [--out DIR]

    <target> names a rig under manifest/<target>/rig.py (single / split / local;
    default ``single``); the rig assembles the services + apps manifests on the
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

    target = next((a for a in args if not a.startswith("-")), _DEFAULT_TARGET)
    attr = next((args[i + 1] for i, a in enumerate(args) if a == "--attr"), "RIG")
    out_arg = next((args[i + 1] for i, a in enumerate(args) if a == "--out"), None)
    out = Path(out_arg) if out_arg else WORKSPACE / _MANIFEST_DIR
    module = f"manifest.{target}.rig"

    cmd = ["artheia", "serialize-manifest", module, "--attr", attr,
           "--out", str(out)]
    if (rc := _run(cmd)) != 0:
        return rc

    machines = json.loads((out / "machines.json").read_text())["machines"]

    # Per-FC static params JSON — config/<fc>.json per machine, the SAME emitter
    # `theia install` uses. Without this the deploy tree had no config/ dir, so
    # `theia orchestrate` (seed-config.yml) skipped per-FC config entirely (e.g.
    # the tsync GPS-backend profile never reached the rig). Derive each machine's
    # fc→target map from its execution.json (serialize-manifest just wrote it).
    for m in machines:
        exec_json = out / m / "execution.json"
        if not exec_json.is_file():
            continue
        procs = json.loads(exec_json.read_text()).get("processes", [])
        binaries = {p["name"]: p["executable"] for p in procs
                    if p.get("executable")}
        if (rc := _emit_fc_config(m, binaries, out / m / "config")) != 0:
            return rc

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
    """Build the per-host .deb deploy bundles from the JSON manifests — NO rig.py.

    Arg: the machines.json path (default dist/manifest/machines.json). Reads it
    for the host list, then for each `target` host reads <host>/machine.json for
    the arch → bazel platform and builds //dist/manifest:<host>_pkg (rules/
    dist_ipk.bzl packs from <host>/application.json, .deb by default). One bazel
    invocation per host so each cross-compiles to its own arch.

    Run `theia manifest` first — if the JSON is missing this fails loudly."""
    import json
    arg = next((a for a in args if not a.startswith("-")), None)
    machines_json = Path(arg) if arg else WORKSPACE / _MANIFEST_DIR / "machines.json"
    if not machines_json.is_file():
        print(f"theia dist: no manifest at {machines_json} — run `theia "
              "manifest` first to emit the JSON deploy manifests.", file=sys.stderr)
        return 1
    mdir = machines_json.parent
    # machines.json is a NAME LIST ({"machines":["central",...]}) in the
    # serialize-manifest shape; every machine is a deploy target.
    machines = json.loads(machines_json.read_text())["machines"]
    rc_final = 0
    for host in machines:
        mj = mdir / host / "machine.json"
        if not mj.is_file():
            print(f"theia dist: missing {mj}", file=sys.stderr)
            return 1
        arch = json.loads(mj.read_text())["arch"]
        platform = _ARCH_PLATFORM.get(arch)
        if not platform:
            print(f"theia dist: {host}: no bazel platform for arch '{arch}'",
                  file=sys.stderr)
            return 1
        # //dist/manifest:<host>_pkg (.deb), cross-compiled for the host's arch.
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
        import shutil
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
]


def _build_framework_deb(out_dir: Path, version: str = "0.1.0") -> int:
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
    if (rc := _run([sys.executable, "-m", "pip", "download",
                    "--dest", str(wheels), "nanopb>=0.4.9", *dep_srcs])) != 0:
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
    for s in ("setup.bash", "setup.zsh"):
        shutil.copy2(pkg_root / s, opt / s)

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


def cmd_release(args: list[str]) -> int:
    """Build the installable Theia package set → dist/debian/ + dist/ipkg/.

    The 4-step build (framework → runtime → services → package) that produces the
    ROS2-style independent packages:
      theia-framework  artheia + deps + rules → .deb (/opt/theia, setup.bash)
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

    if "-h" in args or "--help" in args:
        print(cmd_release.__doc__, file=sys.stderr)
        return 0

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
        #    ROS2-style setup.bash). Arch-independent (Architecture: all). ──────
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


def cmd_release_app(args: list[str]) -> int:
    """Build + publish a USER-WS APP bundle for day-2 Mender OTA (the app plane).

    The runtime/app dichotomy: `theia release [--runtime]` ships the UNIVERSAL
    platform (supervisor + services) to the runtime plane (colony factory-installs
    it). `theia release-app` ships ONLY the user's app FCs — the day-2 delivery
    unit Mender installs as an OVERLAY into the running release (services + the
    supervisor are NEVER touched). The app build is target-arch — the arch is
    DEFINED BY THE FLEET's board hardware.

    Usage:
      theia release-app <app> [opts]
        <app>            the apps/<app> composition (e.g. gateway)
        --app-version V  the app semver (default 0.1.0) — the install dir + Mender
                         artifact name (keyed <app>-<V>)
        --fleet F        the hardware-capability fleet / Mender device-group
                         (default theia-rig) — the S3 app-plane key + Mender
                         --device-type
        --arch A         target arch (default x86_64) — the fleet board's arch
        --machine M      the manifest machine whose app slice to pack (default:
                         the app's host_machine from application.json)
        --s3 URL         publish to the app plane on this MinIO/S3 (e.g.
                         http://10.0.0.99:9000); omit to only build the .mender
        --mender-only    just write dist/apps/<app>/<app>-<V>.mender (no S3 push)

    Output:
      dist/apps/<app>/<app>-<V>.mender   the Mender artifact (theia-app module)
      → S3 user-software/<fleet>/<app>/<V>/  (when --s3 given)

    The artifact payload (consumed by the on-device `theia-app` update module):
      bin/<fc>          the app's FC binaries (the overlay)
      app.json          {name, version, processes[], executor_subtree}
      version.txt       <app>-<V>  (the module's artifact-name gate)
    """
    import json
    import shutil

    if "-h" in args or "--help" in args or not args:
        print(cmd_release_app.__doc__, file=sys.stderr)
        return 0 if args else 2

    app = next((a for a in args if not a.startswith("-")), None)
    if not app:
        print("theia release-app: needs an <app> name (apps/<app>).",
              file=sys.stderr)
        return 2

    def _opt(name, default=None):
        for i, a in enumerate(args):
            if a == name and i + 1 < len(args):
                return args[i + 1]
        return default

    app_ver = _opt("--app-version", "0.1.0")
    fleet = _opt("--fleet", "theia-rig")
    arch = _opt("--arch", "x86_64")
    # The runtime release this app is built against. NO BACKWARD COMPAT — an app
    # depends on EXACTLY ONE runtime+services version (its ABI/proto/runtime are
    # pinned at build time). Recorded in app.json as `requires_runtime`; the GS
    # deploy gate refuses to install onto a device whose base_version differs.
    requires_runtime = _opt("--requires-runtime", "")
    s3_url = _opt("--s3")
    mender_only = "--mender-only" in args or not s3_url
    platform = _ARCH_PLATFORM.get(arch)
    if not platform:
        print(f"theia release-app: no bazel platform for arch '{arch}'.",
              file=sys.stderr)
        return 2

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
        print(f"theia release-app: no application '{app}' in any "
              f"{mdir}/*/application.json — run `theia manifest` on the app rig "
              "first.", file=sys.stderr)
        return 1
    print(f"theia release-app: {app} v{app_ver} fleet={fleet} arch={arch} "
          f"machine={machine} procs={app_procs}", file=sys.stderr)

    # ── Resolve the app's bazel package prefix. The canonical (per-composition)
    #    layout is //apps/<app>/<Composition>/main:<cluster>; the executor records
    #    the "<app>/<Composition>" subtree in each process's `modules`. Fall back
    #    to the flat //apps/<app>/main for legacy single-composition apps. ────────
    ej0 = (mdir / machine / "executor.json") if machine else None
    app_prefix = f"apps/{app}"   # flat fallback
    if ej0 and ej0.is_file():
        mods = _app_module_paths(json.loads(ej0.read_text()), app_procs)
        # mods like {"gateway/GatewayBridge"} → prefix "apps/gateway/GatewayBridge"
        if len(mods) == 1:
            app_prefix = "apps/" + next(iter(mods))
        elif len(mods) > 1:
            print(f"theia release-app: {app} spans multiple compositions {mods} "
                  "— build each separately (--machine / per-composition).",
                  file=sys.stderr)
            return 1

    # ── Build the app's FC binaries (target-arch), cross-compiled to the fleet
    #    arch. The cluster target is the package's last segment. ─────────────────
    fc_targets = [f"//{app_prefix}/main:{p}" for p in app_procs]
    if (rc := _run(["bazel", "build", *fc_targets,
                    f"--platforms={platform}"])) != 0:
        print("theia release-app: app FC build failed.", file=sys.stderr)
        return rc

    # ── Stage the app release tree: bin/<fc> + app.json + version.txt. ─────────
    out_dir = WORKSPACE / "dist" / "apps" / app
    out_dir.mkdir(parents=True, exist_ok=True)
    stage = out_dir / "_stage"
    if stage.exists():
        shutil.rmtree(stage)
    (stage / "bin").mkdir(parents=True)
    bzbin = WORKSPACE / "bazel-bin" / Path(app_prefix) / "main"
    for p in app_procs:
        src = bzbin / p
        if not src.is_file():
            print(f"theia release-app: built binary missing: {src}",
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
            print(f"theia release-app: bad --asset '{a}' (want <src>:<destdir>).",
                  file=sys.stderr)
            return 2
        asrc, adest = a.rsplit(":", 1)
        asrc_p = Path(asrc)
        if not asrc_p.exists():
            print(f"theia release-app: --asset source missing: {asrc_p}",
                  file=sys.stderr)
            return 1
        adest_dir = stage / adest
        adest_dir.mkdir(parents=True, exist_ok=True)
        if asrc_p.is_dir():
            shutil.copytree(asrc_p, adest_dir / asrc_p.name, dirs_exist_ok=True)
        else:
            shutil.copy2(asrc_p, adest_dir / asrc_p.name)
        print(f"theia release-app: staged asset {asrc_p} → {adest}/", file=sys.stderr)

    # The executor subtree for just this app (the supervisor merges it under its
    # tree on install). Pull the app's slice from the machine's executor.json.
    exec_subtree = None
    ej = (mdir / machine / "executor.json") if machine else None
    if ej and ej.is_file():
        full = json.loads(ej.read_text())
        exec_subtree = _extract_app_subtree(full, app, app_procs)
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
                    print(f"theia release-app: bad --env '{spec}' "
                          "(want <proc>:<KEY>=<VAL>).", file=sys.stderr)
                    return 2
                proc, kv = spec.split(":", 1)
                k, v = kv.split("=", 1)
                by_proc.setdefault(proc, {})[k] = v
            for node in exec_subtree.get("nodes", []):
                extra = by_proc.get(node.get("name"))
                if extra:
                    node.setdefault("env", {}).update(extra)
    # ── App ARITY + role names — the app's machine set (manifest machines.json).
    #    arity = how many machines the app spans (single rig = 1; central+compute
    #    split = 2). GS reads this on S3 scan to show app/N + role names and to
    #    drive the per-role Distribution/deploy model. See
    #    project-distribution-deploy-model. (Machine = a manifest role NAME here;
    #    the physical rig + its abi are resolved at deploy.)
    machines_roles: list[str] = []
    mjson = mdir / "machines.json"
    if mjson.is_file():
        try:
            machines_roles = json.loads(mjson.read_text()).get("machines", []) or []
        except Exception:  # noqa: BLE001
            machines_roles = []
    (stage / "app.json").write_text(json.dumps({
        "name": app, "version": app_ver, "fleet": fleet, "arch": arch,
        # The pinned runtime dependency (no backward compat). Empty = unpinned
        # (legacy / arch-only compatibility); the GS deploy gate then only checks
        # arch. A real release SHOULD pass --requires-runtime <key>.
        "requires_runtime": requires_runtime,
        # arity = len(roles); roles = the manifest machine names this app spans.
        "roles": machines_roles, "arity": len(machines_roles) or 1,
        "processes": app_procs, "executor_subtree": exec_subtree,
    }, indent=2))
    artifact_name = f"{app}-{app_ver}"
    (stage / "version.txt").write_text(artifact_name + "\n")

    # ── Pack the Mender artifact (theia-app module). Reuse mender-artifact if
    #    present; else leave the staged tree + a tarball for the GW to pack. ─────
    mender_out = out_dir / f"{artifact_name}.mender"
    tarball = out_dir / f"{artifact_name}.tar.gz"
    import tarfile
    with tarfile.open(tarball, "w:gz") as tf:
        tf.add(stage, arcname=".")
    if shutil.which("mender-artifact"):
        rc = _run([
            "mender-artifact", "write", "module-image",
            "--type", "theia-app",
            "--artifact-name", artifact_name,
            "--device-type", fleet,
            "--file", str(tarball),
            "--file", str(stage / "version.txt"),
            "--file", str(stage / "app.json"),
            "--output-path", str(mender_out),
        ])
        if rc != 0:
            print("theia release-app: mender-artifact pack failed.",
                  file=sys.stderr)
            return rc
        print(f"theia release-app: wrote {mender_out}", file=sys.stderr)
    else:
        print("theia release-app: mender-artifact not installed — staged tree + "
              f"{tarball} written; pack on the GW.", file=sys.stderr)

    # ── Publish to the app plane: user-software/<fleet>/<app>/<ver>/. ──────────
    if not mender_only and s3_url:
        rc = _publish_app_plane(s3_url, fleet, app, app_ver,
                                mender_out if mender_out.exists() else None,
                                tarball, stage / "app.json")
        if rc != 0:
            return rc
    return 0


def _extract_app_subtree(executor: dict, app: str, procs: list[str]):
    """Return the executor.json subtree (the supervisor child nodes) for just the
    app's processes — what the on-device app module merges into the running tree.
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


def _app_module_paths(executor: dict, procs: list[str]) -> set:
    """The set of "<app>/<Composition>" bazel-package subtrees for the app's
    processes, read from each process node's `modules` (the executor records the
    composition path there, e.g. "gateway/GatewayBridge"). Used to derive the
    //apps/<app>/<Composition>/main target for the canonical per-composition
    layout (vs the flat //apps/<app>/main). Empty set → fall back to flat."""
    procset = set(procs)
    mods: set = set()

    def walk(node):
        if isinstance(node, dict):
            if node.get("name") in procset and node.get("start_cmd"):
                for m in node.get("modules", []) or []:
                    if isinstance(m, str) and "/" in m:
                        mods.add(m)
            for k in ("children", "workers", "nodes"):
                for c in node.get(k, []) or []:
                    walk(c)
    walk(executor)
    return mods


# The mender-artifact CLI (the build host's .mender packer). Prefer the
# OpenSSL-1.1-shim wrapper (taycann runs OpenSSL 3; mender-artifact links 1.1)
# if present, else the bare binary.
def _mender_artifact_bin() -> "str | None":
    for cand in ("mender-artifact-wrap", "mender-artifact"):
        if shutil.which(cand):
            return cand
    return None


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
        --version V      the release version (default 0.2.1)
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
    ver = _opt("--version", "0.2.1")
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


def _publish_app_plane(s3_url: str, fleet: str, app: str, ver: str,
                       mender: "Path | None", tarball: "Path",
                       app_json: "Path") -> int:
    """Push the app bundle to the S3 app plane user-software/<fleet>/<app>/<ver>/.
    Uses the aws cli (S3-compatible) against MinIO. Writes an index.json the GW /
    ground-station reads to drive a Mender deployment."""
    import json
    import os
    import subprocess
    if not shutil.which("aws"):
        print("theia release-app: aws cli not found — cannot push to S3 (build "
              "the artifact, push from a host that has it).", file=sys.stderr)
        return 1
    key = f"user-software/{fleet}/{app}/{ver}"
    env = {**os.environ,
           "AWS_ACCESS_KEY_ID": os.environ.get("MINIO_USER", "theia"),
           "AWS_SECRET_ACCESS_KEY": os.environ.get("MINIO_PASSWORD", "theiaminio"),
           "AWS_DEFAULT_REGION": "us-east-1"}
    bucket = "theia-apps"
    aws = ["aws", "--endpoint-url", s3_url, "s3"]

    def _aws(argv: list, check: bool = True) -> int:
        print(f"$ {' '.join(argv)}", file=sys.stderr)
        return subprocess.run(argv, env=env).returncode if not check else \
            subprocess.run(argv, env=env, check=False).returncode

    subprocess.run([*aws, "mb", f"s3://{bucket}"], env=env)  # idempotent
    objs = []
    for f in (mender, tarball, app_json):
        if f and f.is_file():
            if _aws([*aws, "cp", str(f), f"s3://{bucket}/{key}/{f.name}"]) != 0:
                return 1
            objs.append(f.name)
    # surface the runtime dependency in the index so the catalog reads it without
    # fetching the full app.json (no backward compat — app pins ONE runtime).
    requires_runtime = ""
    try:
        requires_runtime = json.loads(app_json.read_text()).get("requires_runtime", "")
    except Exception:  # noqa: BLE001
        pass
    idx = {"plane": "app", "fleet": fleet, "app": app, "version": ver,
           "artifact": (mender.name if mender and mender.is_file() else None),
           "requires_runtime": requires_runtime,
           "files": objs}
    idx_path = WORKSPACE / "dist" / "apps" / app / f"index-{ver}.json"
    idx_path.write_text(json.dumps(idx, indent=2))
    if _aws([*aws, "cp", str(idx_path), f"s3://{bucket}/{key}/index.json"]) != 0:
        return 1
    print(f"theia release-app: published → s3://{bucket}/{key}/", file=sys.stderr)
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

    --with-services bootstraps the workspace with the platform services: it links
    system/services and emits a rig built on the framework's ServicesSoftware, so
    a bare `theia start` brings the full service tree up under the supervisor.

    It creates, in the CWD (never overwriting an existing file):
      - system/apps/{package,component}.art — this workspace's OWN app package,
        a REAL dir (FQN system.apps ↔ this path 1:1; no symlink). You edit these.
      - system/system.art   — the workspace aggregator. Imports the Theia
        clusters (services / supervisor) you'll deploy, plus a stub you fill in
        by hand (link system/<yourthing> + add its cluster). You then `theia
        manifest` against it.
      - manifest/bootstrap/rig.py — the one-machine BOOTSTRAP rig (smoke-test a
        fresh workspace via `theia manifest bootstrap`); imports the generated
        apps manifest (gen-manifest writes manifest/apps/manifest.py).
      - apps/, proto/        — homes for the GENERATED C++ (gen-app --out apps)
        and proto (gen-app --proto-out proto); never mixed with the framework.
      - .theia               — records THEIA_ROOT (the source it's bound to).

    Re-runnable: `theia init` is idempotent — it never overwrites your files
    (system/apps, impl/), only (re)links the framework deps + (re)writes the
    scaffold BUILD/shim files. Run it again (e.g. add --with-services) any time.

    Theia itself is NOT vendored: system/platform/runtime + system/supervisor
    (and, with --with-services, system/services) are SYMLINKS into $THEIA_ROOT,
    so a Theia bump is a re-source, not a re-copy."""
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
    # THEIA_ROOT is either a SOURCE checkout (system/system.art present) or an
    # INSTALLED prefix (/opt/theia from the debs, a different on-disk layout).
    # Resolve each framework .art root for whichever this is, so the symlinks we
    # plant in the workspace point at real files the artheia resolver can reach.
    src = (theia_root / "system" / "system.art").is_file()
    if src:
        # Source checkout: the whole .art tree is real files under system/, with
        # the on-disk layout mirroring each package FQN 1:1 (the runtime is
        # `package system.platform.runtime`, hence system/platform/runtime/).
        runtime_pkg = theia_root / "system" / "platform" / "runtime"
        runtime_art = runtime_pkg / "package.art"
        supervisor_pkg = theia_root / "system" / "supervisor"
        services_pkg = theia_root / "system" / "services"
        msgs_pkg = theia_root / "system" / "platform" / "msgs"
    else:
        # Installed deb layout (theia-runtime-dev + theia-services-dev):
        #   runtime spec  → system/platform/runtime/package.art
        #   supervisor    → system/supervisor/
        #   services tree → services/{cluster.art, <fc>/...}  (deb strips the
        #                   system/services prefix → /opt/theia/services)
        runtime_pkg = theia_root / "system" / "platform" / "runtime"
        runtime_art = runtime_pkg / "package.art"
        supervisor_pkg = theia_root / "system" / "supervisor"
        services_pkg = theia_root / "services"
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
    for i, a in enumerate(args):
        if a == "--name" and i + 1 < len(args):
            name = args[i + 1]
    # --with-services: bootstrap with the ARA platform services (com/log/per/sm/
    # ucm/shwa). Links system/services + the rig builds on ServicesSoftware, so a
    # bare `theia start` brings the full service tree up under the supervisor.
    with_services = "--with-services" in args
    if theia_root == ws:
        print("theia init: refusing to init the Theia checkout itself "
              "(run from your CONSUMING workspace dir).", file=sys.stderr)
        return 2

    created: list[str] = []

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

    # The runtime .art package. The framework's supervisor/services .art import
    # `system.platform.runtime.*` (ChildControlIf, TraceControlPush, LogLevelPush);
    # the resolver maps that FQN to system/platform/runtime/, so link the package
    # dir there. (One link, FQN-correct — no separate platform/runtime/package.art
    # or system/runtime shims.)
    _link("system/platform/runtime", runtime_pkg)
    # The supervisor .art (system.supervisor). Present in a source checkout and in
    # the theia-runtime-dev deb; link it so `import system.supervisor.*` resolves.
    if supervisor_pkg.exists():
        _link("system/supervisor", supervisor_pkg)
    # --with-services: link the framework's ARA service FCs so `cluster Services`
    # resolves + the rig can import manifest.services.{manifest,executor}. The
    # service .art's import `system.platform.msgs.{std,geometry,sensor,nav}.*` (e.g.
    # tsync uses nav.GnssSolution), so link the msgs namespace dir too — one link
    # covers all four subpackages (FQN system.platform.msgs.<x> → msgs/<x>/). Without
    # it a service that references a platform msg fails to resolve (Unknown object).
    if with_services:
        _link("system/services", services_pkg)
        if msgs_pkg.exists():
            _link("system/platform/msgs", msgs_pkg)
    # The workspace's OWN empty app package (no compositions yet). gen-manifest
    # walks `cluster Applications` here — empty → an empty app manifest +
    # executor sidecar, which the rig imports as-is.
    #
    # system/apps is the REAL, canonical app source (FQN system.apps maps to the
    # dir 1:1 — no `apps/system/apps` indirection, no symlink). The user edits
    # these; gen-app emits the C++ to apps/ and the proto to proto/, gen-manifest
    # writes the Python sidecar to manifest/ — all SEPARATE from this source dir.
    _write("system/apps/package.art", _INIT_APPS_PACKAGE_ART)
    _write("system/apps/component.art", _INIT_APPS_COMPONENT_ART)
    # Python package marker for the generated C++ tree. NOTE: `manifest/` is
    # deliberately a PEP-420 NAMESPACE package (NO manifest/__init__.py) so the
    # `manifest` namespace spans BOTH this workspace (manifest.apps / manifest.rig)
    # AND the framework root (manifest.services) — a regular __init__.py here would
    # shadow manifest.services and break --with-services.
    _write("apps/__init__.py", "")

    sys_art = (_INIT_SYSTEM_ART_SERVICES if with_services else _INIT_SYSTEM_ART)
    rig_py = (_INIT_RIG_PY_SERVICES if with_services else _INIT_RIG_PY)
    _write("system/system.art", sys_art.replace("@NAME@", name))
    # The BOOTSTRAP rig — a one-machine smoke-test target for verifying a fresh
    # workspace's toolchain before it has real deploy targets. Lives at
    # manifest/bootstrap/rig.py so it's addressable as `theia manifest bootstrap`
    # (manifest.<target>.rig) and sits beside, not in competition with, the real
    # per-target rigs (manifest/single/rig.py, …) a workspace grows later.
    _write("manifest/bootstrap/__init__.py", "")
    _write("manifest/bootstrap/rig.py", rig_py.replace("@NAME@", name))
    # Record THEIA_ROOT RELATIVE to the workspace when they share a prefix (keeps
    # the ws+theia pair relocatable, e.g. an in-repo demo/ committed with a
    # `../` link); fall back to absolute if they're on different roots.
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
    _write(".bazelversion", _read_or(theia_root / ".bazelversion", "8.0.0"))
    # The app's own proto package: gen-app writes apps.proto + apps.options under
    # proto/system/apps/ (--proto-out proto); this BUILD nanopb-compiles them.
    # //proto:platform_protos aggregates it (+ the runtime proto from @pero_theia)
    # so the gen-app lib's `//proto:platform_protos` dep resolves locally. The app
    # proto lives under proto/ (the workspace's own), NOT platform/proto/ (which
    # in the framework holds the FC protos) — they never mix.
    _write("proto/BUILD.bazel", _INIT_PROTO_AGG)
    _write("proto/system/apps/BUILD.bazel", _INIT_APPS_PROTO_BUILD)

    flavour = "services workspace" if with_services else "empty workspace"
    print(f"\ntheia init: scaffolded '{name}' ({flavour}) against {theia_root}",
          file=sys.stderr)
    for c in created:
        print(f"  + {c}", file=sys.stderr)
    extra = ("\n  (the ARA services com/log/per/sm/ucm/shwa come up under the "
             "supervisor)" if with_services else "")
    print("\nVerify the toolchain before adding apps (the bootstrap rig):\n"
          "  artheia gen-manifest system/apps/component.art "
          "manifest/apps/manifest.py\n"
          f"  theia manifest bootstrap && theia install && theia start{extra}\n"
          "\nThen add a composition to system/apps/component.art and "
          "generate + build its C++:\n"
          "  artheia gen-app --kind fc system/apps/component.art "
          "--out apps --proto-out proto\n"
          "  bazel build //apps/...        # compiles against @pero_theia",
          file=sys.stderr)
    return 0


_INIT_SYSTEM_ART = '''\
// @NAME@ — Theia consuming-workspace aggregator.
//
// Imports the Theia supervisor + THIS workspace's own app package (resolved
// through the system/ symlinks). `theia manifest` walks this file.
//
// EMPTY-workspace shape: just the supervisor + an (empty) Applications cluster.
// Add your app by declaring a `composition` in system/apps/component.art and
// `cluster Applications { composition <Yours> <id> }` — it flows here via the
// import below, no edit needed.

package system

import system.supervisor.*   // the OTP-style supervisor (the runtime fabric)
import system.apps.*         // THIS workspace's app package (system/apps, real dir)

// --- forward-decl: the clusters this workspace deploys --------------------
cluster Applications { }     // empty until you add a composition in system/apps/

composition Supervisor { }

// --- this workspace's deployment ------------------------------------------
cluster Platform {
    composition Supervisor  sup
}
'''

_INIT_APPS_PACKAGE_ART = '''\
// @NAME@ apps — message + node declarations for this workspace's applications.
//
// EMPTY scaffold. Declare your nodes here (messages, interfaces, `node atomic
// <Name> { tipc ... ports { ... } }`), then prototype them in a composition in
// the sibling component.art. Until then this package is valid-but-empty so the
// toolchain (parse / gen-manifest / build / run a bare supervisor) works before
// you write a single app.

package system.apps
'''

_INIT_APPS_COMPONENT_ART = '''\
// @NAME@ apps — composition + cluster wiring.
//
// EMPTY scaffold: `cluster Applications { }` with no members. gen-manifest
// emits an empty app manifest + executor sidecar from this, which the rig
// imports as-is — so `theia manifest`/`install`/`start` all run on an empty
// workspace (bare supervisor, no app children).
//
// To add an app:
//   1. declare a node in package.art
//   2. `composition MyApp { prototype MyNode my_node }`  (here)
//   3. `cluster Applications { composition MyApp my_app }`  (here)
//   4. `artheia gen-app --kind fc system/apps/component.art --out apps
//       --proto-out proto [--composition MyApp]` to emit the C++
//       (--proto-out lands apps.proto where proto/'s BUILD expects it),
//       then `bazel build //apps/...` (compiles against @pero_theia).

package system.apps

cluster Applications { }
'''

_INIT_RIG_PY = '''\
"""@NAME@ BOOTSTRAP rig — one machine ("central") running this workspace's apps.

The smoke-test target for a FRESH workspace: it lets `theia manifest bootstrap
&& theia install && theia start` run before you have any real deploy targets,
so you can verify the toolchain end to end on a clean scaffold. Addressed as
`bootstrap` because it lives at manifest/bootstrap/rig.py (manifest.<target>.rig);
the real per-target rigs (manifest/single/rig.py, …) come later and sit beside
it, never replacing it.

A :class:`DeploymentLayer` on the orthogonal-ARA engine
(:mod:`artheia.manifest.deployment`). It combines the workspace's generated
apps manifest (the BASE — open machines) with a deploy delta: one machine and
every process bound to it. `theia manifest bootstrap` reads the RIG export.

The apps manifest is gen-manifest output. Until you run it the import fails, so
it is guarded — a fresh workspace resolves to an EMPTY deployment (one machine,
no processes), which is enough to verify the toolchain. As you add compositions
to system/apps/component.art and regenerate (`artheia gen-manifest
system/apps/component.art manifest/apps/manifest.py`), the processes +
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
    from manifest.apps.manifest import DEPLOYMENT as _APPS
except Exception:               # not generated yet → empty workspace
    _APPS = DeploymentLayer()

# Every process the apps base declares (bind each to the one machine below).
from artheia.manifest.deployment import _members as _set_members
_PROCESS_NAMES = sorted(p.name for p in _set_members(_APPS.execution.processes))

# The deploy delta: one machine "central"; every app process bound to it; one
# AA ("apps") grouping them on that host. Combined onto the apps base.
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
        Append(ApplicationLayer(name="apps", host_machine=Explicit("central"))),
    }),
))

# Optional supervisor sidecar (gen-manifest writes manifest/apps/executor.py).
# serialize-manifest reads SUPERVISORS off this module if present.
try:
    from manifest.apps.executor import SUPERVISORS
except Exception:
    SUPERVISORS = []

# Per-process node/module metadata for the executor.json worker leaves.
# gen-manifest emits PROCESS_NODES onto manifest.apps.manifest.
try:
    from manifest.apps.manifest import PROCESS_NODES
except Exception:
    PROCESS_NODES = {}
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
import system.apps.*         // THIS workspace's app package

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
`bootstrap` (manifest/bootstrap/rig.py): `theia manifest bootstrap && theia
install && theia start` verifies the toolchain before real deploy targets exist.

A :class:`DeploymentLayer` (orthogonal-ARA engine) built by combining the
framework's services manifest (the full FC set: com/log/per/sm/ucm/shwa…) with
this workspace's generated apps manifest, then a deploy delta binding everything
to one machine ("central"). `theia install` builds the FC binaries + the
supervisor; `theia start` runs the whole service tree with your apps under it.

As you add compositions to system/apps/component.art and regenerate
(`artheia gen-manifest system/apps/component.art manifest/apps/manifest.py`),
the app processes + applications flow in automatically.

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
    _members as _set_members,
)

# The framework's ARA services manifest (a base DeploymentLayer, machines open).
from manifest.services.manifest import DEPLOYMENT as _SERVICES

# This workspace's generated apps manifest (empty until you add apps). Guarded:
# a fresh workspace has no manifest/apps/manifest.py yet → services-only.
try:
    from manifest.apps.manifest import DEPLOYMENT as _APPS
except Exception:               # not generated yet → services-only
    _APPS = DeploymentLayer()

# The assembled base: services ⊕ apps (machines still open).
_BASE = _SERVICES.combine(_APPS)
_PROCESS_NAMES = sorted(p.name for p in _set_members(_BASE.execution.processes))

# The deploy delta: one machine "central"; every process bound to it; two AAs
# (the platform services + this workspace's apps) hosted on it.
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
        Append(ApplicationLayer(name="services", host_machine=Explicit("central"))),
        Append(ApplicationLayer(name="apps", host_machine=Explicit("central"))),
    }),
))

# Merged supervisor sidecar (services + apps executor trees under one root).
# Read by serialize-manifest off this module if present.
try:
    from artheia.manifest.supervisor import RestartStrategy, SupervisorNode
    from manifest.services.executor import SUPERVISORS as _SVC_SUP
    try:
        from manifest.apps.executor import SUPERVISORS as _APP_SUP
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
    from manifest.services.manifest import PROCESS_NODES as _SVC_NODES
    try:
        from manifest.apps.manifest import PROCESS_NODES as _APP_NODES
    except Exception:
        _APP_NODES = {}
    PROCESS_NODES = {**_SVC_NODES, **_APP_NODES}
except Exception:
    PROCESS_NODES = {}
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
not vendored. system/platform/runtime + system/supervisor (+ system/services
with --with-services) are symlinks into it.

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
'''

_INIT_BAZELRC = '''\
# Consuming-workspace Bazel config (mirrors the framework, host-only).
build --enable_bzlmod
build --incompatible_enable_cc_toolchain_resolution
build --action_env=PATH
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
# workspace builds its OWN app proto (system/apps, nanopb-compiled below) + pulls
# the runtime control proto from @pero_theia. (The framework aggregates all the
# FC protos under //platform/proto; a consuming workspace only needs its own app
# proto + the runtime one — the lib #includes "system/apps/apps.pb.h". The app
# proto lives under proto/, never mixed with the framework's platform/proto/.)
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "platform_protos",
    srcs = ["//proto/system/apps:apps_pb_c"],
    hdrs = ["//proto/system/apps:apps_pb_h"],
    includes = ["."],   # callers #include "system/apps/apps.pb.h"
    copts = ["-fPIC"],
    deps = ["@pero_theia//platform/runtime:runtime_proto_cc"],
)
'''

_INIT_APPS_PROTO_BUILD = '''\
# nanopb sources for the system.apps package. gen-app (--proto-out proto)
# writes apps.proto AND apps.options here (it auto-pins every string/bytes field
# to a fixed char[]; override per field with an .art `[max_size:N]`). Both feed
# this genrule (.options auto-loaded by nanopb). .pb.{c,h} are BUILT, not committed.
package(default_visibility = ["//visibility:public"])

genrule(
    name = "apps_pb",
    srcs = ["apps.proto"] + glob(["apps.options"], allow_empty = True),
    outs = ["apps.pb.c", "apps.pb.h"],
    cmd = "set -e;"
        + " in_dir=$$(dirname $(location apps.proto));"
        + " out_dir=$$(dirname $(location apps.pb.c));"
        + " nanopb_generator -I $$in_dir -D $$out_dir apps.proto;",
    local = True,
)
filegroup(name = "apps_pb_c", srcs = ["apps.pb.c"])
filegroup(name = "apps_pb_h", srcs = ["apps.pb.h"])
filegroup(name = "apps_proto", srcs = ["apps.proto"])
'''


COMMANDS = {
    "init":        (cmd_init,        "scaffold the CWD as a Theia consuming workspace (source or /opt/theia)"),
    "rig":         (cmd_rig,         "docker compose {up|down} the deploy stack"),
    # provision/orchestrate/cleanup MOVED to the `colony` repo (deploy adapter).
    # theia emits the per-rig bundle via `manifest`/`dist`; colony deploys it.
    "install":     (cmd_install,     "build + populate install/<machine>/ (local host)"),
    "stage-local": (cmd_install,     "alias for `install` (back-compat)"),
    "start":       (cmd_start,       "run the staged supervisor from install/<machine>/ (detached + pidfile)"),
    "stop":        (cmd_stop,        "stop the supervisor started by `theia start` (graceful)"),
    "manifest":    (cmd_manifest,    "rig.py → dist/manifest/*.json (sole rig entry for deploy)"),
    "dist":        (cmd_dist,        "per-host .ipk from dist/manifest/ JSON (no rig.py)"),
    "release":     (cmd_release,     "build the installable package set (.deb+.ipk) → dist/debian + dist/ipkg"),
    "release-app": (cmd_release_app, "build + publish a user-ws app bundle (day-2 Mender OTA, the app plane)"),
    "release-role": (cmd_release_role, "build + publish a per-board role .mender (L4-C vehicle campaign)"),
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

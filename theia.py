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
    theia provision       puppet apply  — Phase 1 (install os pkgs + .ipk)
    theia orchestrate     puppet apply  — Phase 2 (app rollout, no restart)
    theia manifest        rig.py → dist/manifest/*.json (the SOLE rig entry)
    theia dist            per-host .ipk from dist/manifest/ JSON (no rig.py)
    theia install         build + puppet-populate install/<machine>/ (local host)
                          + manifests → install/manifest/<machine>/
    theia compdb          regen compile_commands.json from bazel (for clangd)

Extra args after the verb pass through (e.g. `theia rig up central`,
`theia install compute`).
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

# THEIA_ROOT — the framework checkout (where theia.py lives, or $THEIA_ROOT when
# a consuming workspace sourced setup.sh). Framework assets (deploy/, rules/)
# resolve against it.
THEIA_ROOT = Path(os.environ.get("THEIA_ROOT") or Path(__file__).resolve().parent)

# WORKSPACE — the directory the user RAN `theia` from. For a consuming workspace
# (gataway_ws, test_ws) that's the workspace itself; the rig, dist/manifest, and
# install/ live HERE, not in the framework. THEIA_INVOCATION_CWD is set by main()
# before it chdir's; falls back to THEIA_ROOT when run inside the framework repo.
WORKSPACE = Path(
    os.environ.get("THEIA_INVOCATION_CWD") or os.getcwd()
).resolve()
# Running inside the framework checkout itself → workspace IS the framework.
if not (WORKSPACE / "manifest").is_dir() and not (WORKSPACE / ".theia").is_file() \
        and (THEIA_ROOT / "apps").is_dir():
    # No consuming-workspace markers here and the framework tree is under us:
    # treat the framework as the workspace (the in-repo dev path, unchanged).
    if WORKSPACE == THEIA_ROOT or str(WORKSPACE).startswith(str(THEIA_ROOT)):
        WORKSPACE = THEIA_ROOT

COMPOSE = THEIA_ROOT / "deploy" / "docker-compose.yml"
PUPPET = THEIA_ROOT / "deploy" / "puppet"


def _run(argv: list[str], cwd: "Path | None" = None) -> int:
    """Run a command (default cwd = WORKSPACE), streaming output; return rc.

    Injects WORKSPACE onto PYTHONPATH so a subprocess (`artheia
    generate-manifest manifest.rig`) can import the workspace's own rig +
    generated manifest modules (manifest.rig imports apps.manifest.app). Python
    doesn't put cwd on sys.path for console-script entry points, so we do it.

    `cwd` overrides the working dir — e.g. a framework bazel build runs from
    THEIA_ROOT when the consuming workspace has no MODULE.bazel of its own."""
    print(f"$ {' '.join(argv)}", file=sys.stderr)
    env = os.environ.copy()
    ws = str(WORKSPACE)
    env["PYTHONPATH"] = ws + (os.pathsep + env["PYTHONPATH"]
                              if env.get("PYTHONPATH") else "")
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


def _puppet(manifest: str, args: list[str]) -> int:
    return _run([
        "puppet", "apply",
        f"--modulepath={PUPPET / 'modules'}",
        str(PUPPET / manifest),
        f"--hiera_config={PUPPET / 'hiera.yaml'}",
        *args,
    ])


def cmd_provision(args: list[str]) -> int:
    """puppet apply Phase 1 — install OS packages + .ipk artifacts.
    Reads /etc/theia/manifest/machine.yaml; target from $THEIA_MACHINE."""
    return _puppet("provisioning.pp", args)


def cmd_orchestrate(args: list[str]) -> int:
    """puppet apply Phase 2 — day-to-day app rollout, no supervisor restart."""
    return _puppet("orchestration.pp", args)


def _bazel_root() -> Path:
    """The dir bazel builds run in. A consuming workspace with its OWN
    MODULE.bazel (e.g. gataway_ws → @pero_theia) builds in place; an empty /
    no-bazel workspace builds the framework targets (//platform/...) from
    THEIA_ROOT (the framework module). So: WORKSPACE if it has MODULE.bazel,
    else THEIA_ROOT."""
    return WORKSPACE if (WORKSPACE / "MODULE.bazel").is_file() else THEIA_ROOT


def _bazel_bin(target: str) -> Path:
    """//pkg/dir:name -> <bazel-root>/bazel-bin/pkg/dir/name."""
    pkg, name = target.lstrip("/").split(":", 1)
    return _bazel_root() / "bazel-bin" / pkg / name


def _discover_rig_module(zonal: bool = False) -> "str | None":
    """Find the workspace's rig Python module — name-INDEPENDENT, so Theia works
    in the monorepo (apps.manifest.rig) AND a downstream workspace (manifest.rig)
    with no hardcoded name.

    Resolution order:
      1. $THEIA_RIG_MODULE (zonal: $THEIA_ZONAL_RIG_MODULE) — explicit override.
      2. The single `*/manifest/rig.py` (or zonal_rig.py) under the workspace
         → its dotted module (apps/manifest/rig.py → apps.manifest.rig;
         manifest/rig.py → manifest.rig).
    Returns None if none/ambiguous (caller errors helpfully)."""
    leaf = "zonal_rig.py" if zonal else "rig.py"
    env = os.environ.get(
        "THEIA_ZONAL_RIG_MODULE" if zonal else "THEIA_RIG_MODULE")
    if env:
        return env
    # Trees that are not the WORKSPACE's deploy rig: vendored repos, the venv,
    # bazel/build outputs, the framework's own internals (artheia.manifest.*),
    # and the shipped workspace template.
    _skip = {"up", ".venv", "bazel-bin", "bazel-out", "external", "vendor",
             "artheia", "templates", "build", ".git"}
    hits = []
    for p in WORKSPACE.glob(f"**/manifest/{leaf}"):
        rel = p.relative_to(WORKSPACE)
        if any(seg in _skip for seg in rel.parts):
            continue
        # dotted module = the path minus the .py, dirs joined by '.'
        hits.append(".".join(rel.with_suffix("").parts))
    if len(hits) == 1:
        return hits[0]
    if not hits:
        return None
    # Ambiguous: prefer a top-level `manifest.<leaf>` (the downstream convention).
    top = f"manifest.{leaf[:-3]}"
    return top if top in hits else None


def _load_install_components(manifest_root: Path):
    """Read the generated AUTOSAR application.json and return what to build/stage.

    The deploy MANIFEST is the single source of truth — no hardcoded binary
    list. `artheia generate-manifest` writes
    ``<manifest_root>/<host>/application.json`` whose ``applications[].components[]``
    each carry ``name`` (the staged ``bin/<name>`` leaf), ``bazel_target`` (the
    label), ``owner`` (``apps``/``services``/``platform``) and ``bazel_buildable``.

    Returns ``(supervisor_target, {name: target})`` over the buildable components,
    with the ``supervisor`` entry split out (it lands at ``<dest>/supervisor``,
    not ``bin/``). Raises if no application.json is found."""
    import json

    appjsons = sorted(manifest_root.glob("*/application.json"))
    if not appjsons:
        raise FileNotFoundError(
            f"no application.json under {manifest_root} — "
            f"`artheia generate-manifest` must run first")
    data = json.loads(appjsons[0].read_text())
    supervisor_target = None
    binaries: dict[str, str] = {}
    for app in data.get("applications", []):
        for c in app.get("components", []):
            if not c.get("bazel_buildable", True):
                continue
            name, target = c.get("name"), c.get("bazel_target")
            if not name or not target:
                continue
            if name == "supervisor":
                supervisor_target = target
            else:
                binaries[name] = target
    if supervisor_target is None:
        raise ValueError(
            f"{appjsons[0]} has no 'supervisor' component — the rig must "
            f"include the supervisor in its application set")
    return supervisor_target, binaries


def _fc_art_path(fc: str, target: str):
    """The .art the gen-params emitter reads for an FC, derived from the bazel
    target. Services FCs (``//services/<fc>/...``) live at the canonical symlink
    path system/services/<fc>/package.art; app compositions (``//apps/...``) at
    system/demo/package.art. Returns a Path or None if absent (an FC with no
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

    log = dest / "supervisor.log"
    env = {
        **os.environ,
        "THEIA_SUPERVISOR_MANIFEST": "executor.json",
        "THEIA_ROOT_DIR": ".",
        "THEIA_SUPERVISOR_INSTANCE": instance,
    }
    # Detach into its own session so it outlives this process; redirect output
    # to the log. start_new_session=True == setsid → the supervisor leads its
    # own session (its children already setsid per-worker).
    logf = open(log, "ab")
    proc = subprocess.Popen(
        ["./supervisor"], cwd=str(dest), env=env,
        stdout=logf, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
        start_new_session=True,
    )
    pidfile.write_text(str(proc.pid) + "\n")
    print(f"theia: supervisor up (pid {proc.pid}, instance {instance}) — "
          f"log: {log}", file=sys.stderr)
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

    Generates the schema + config-defaults from system/demo/package.art and runs
    migration/seed.py's idempotent `defaults` action. per must be up and reachable
    (give it a moment after the supervisor forks it). Best-effort: any failure is
    logged, never fatal."""
    import tempfile
    import time

    art = WORKSPACE / "system" / "demo" / "package.art"
    if not art.exists():
        return
    tmp = Path(tempfile.gettempdir())
    schema = tmp / "theia_seed_schema.json"
    defs   = tmp / "theia_seed_defaults.json"
    try:
        if _run(["artheia", "gen-schema", str(art), "--out", str(schema)]) != 0:
            print("theia: seed skipped — gen-schema failed.", file=sys.stderr)
            return
        if _run(["artheia", "gen-config-defaults",
                 str(art), "--out", str(defs)]) != 0:
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


def cmd_install(args: list[str]) -> int:
    """LOCAL install: build + populate $WORKSPACE/install/<machine>/ via puppet.

    The dev inner-loop counterpart of the remote .ipk deploy, and the inherited
    home of apps/stage_local.sh. The deploy MANIFEST is the single source of
    truth — `artheia generate-manifest` first emits the AUTOSAR manifests, then
    the buildable binary set (supervisor + every child) is READ BACK from
    application.json (no hardcoded binary list). bazel builds those targets +
    artheia emits executor.json + per-FC params; then `puppet apply
    theia::local_install` copies them into install/<machine>/ and applies the
    SAME setcap contract (theia::postinstall) a real deploy uses — "bazel builds,
    puppet orchestrates the host". Default machine: central. Pass a machine name
    to override.

    (`theia stage-local` is a back-compat alias for this verb.)"""
    machine = next((a for a in args if not a.startswith("-")), None)
    # Rig attr: $THEIA_RIG if set, else let the resolver auto-rank (*Software /
    # *Rig). No hardcoded "CentralRig" — a consuming workspace names its own.
    rig = os.environ.get("THEIA_RIG")
    manifest_root = WORKSPACE / "install" / "manifest"

    # Name-independent rig discovery — apps.manifest.rig in the monorepo,
    # manifest.rig in a downstream workspace, or $THEIA_RIG_MODULE. No hardcoded
    # module name (so `theia` carries no project identity).
    rig_module = _discover_rig_module()
    if rig_module is None:
        print("theia: no rig found — expected a `manifest/rig.py` (or set "
              "$THEIA_RIG_MODULE). A workspace declares its deploy via "
              "manifest/rig.py.", file=sys.stderr)
        return 1

    # 0. Address-collision gate — fail BEFORE building/staging if two nodes
    #    share a TIPC (type, instance) anywhere in the deployed FC set.
    if (rc := _check_tipc_addresses()) != 0:
        return rc

    # 1. The four AUTOSAR manifest kinds (machine/application/service/
    #    execution.json) per machine → install/manifest/<host>/. This is the
    #    source of truth for WHAT to build/stage — runs FIRST so the binary set
    #    is derived from it, not hand-listed. Rig module discovered above.
    if (rc := _run([
        "artheia", "generate-manifest", rig_module,
        "--out", str(manifest_root),
    ])) != 0:
        return rc

    # 1a. Resolve the machine to install: the arg, else $THEIA_MACHINE, else the
    #     SINGLE target machine in machines.json (a single-host workspace needs no
    #     name). No hardcoded "central".
    if machine is None:
        import json as _json
        machine = os.environ.get("THEIA_MACHINE")
        if machine is None:
            try:
                ms = _json.loads((manifest_root / "machines.json").read_text())["machines"]
                targets_m = [m["name"] for m in ms if m.get("kind") != "host"]
                if len(targets_m) == 1:
                    machine = targets_m[0]
                elif not targets_m:
                    machine = ms[0]["name"] if ms else "central"
                else:
                    print(f"theia install: multiple machines {targets_m} — pass "
                          "one (e.g. `theia install central`) or set $THEIA_MACHINE.",
                          file=sys.stderr)
                    return 2
            except (FileNotFoundError, KeyError, IndexError):
                machine = "central"
    dest = WORKSPACE / "install" / machine

    # 1b. Read the buildable binary set from the manifest: {bin-name: target}
    #     plus the supervisor (split out — it lands at <dest>/supervisor).
    try:
        supervisor_target, binaries = _load_install_components(manifest_root)
    except (FileNotFoundError, ValueError) as e:
        print(f"theia: {e}", file=sys.stderr)
        return 1

    # 2. bazel build — the supervisor + every child binary, all from the manifest.
    #    Framework targets (//platform/...) build from THEIA_ROOT when the
    #    workspace has no MODULE.bazel of its own (empty / pure-consumer ws).
    targets = [supervisor_target, *binaries.values()]
    if (rc := _run(["bazel", "build", *targets], cwd=_bazel_root())) != 0:
        return rc

    # 3. executor.json — the supervisor tree for this machine (same discovered
    #    rig; --rig optional → resolver auto-ranks; --machine slices this host).
    dest.mkdir(parents=True, exist_ok=True)
    _emit = ["artheia", "executor", "emit", rig_module,
             "--machine", machine, "--out", str(dest / "executor.json")]
    if rig:
        _emit += ["--rig", rig]
    if (rc := _run(_emit)) != 0:
        return rc

    # 3b. Per-FC static params JSON — config/<fc>.json, one per FC that declares
    #     a params {} block. Read once at boot by the runtime config singleton
    #     (init_config(<fc>) resolves $THEIA_CONFIG_DIR/<fc>.json; the supervisor
    #     sets THEIA_CONFIG_DIR=config in the child env, see executor emit). A
    #     params-less FC emits an empty {nodes:{}} (harmless; lookups default).
    cfg_dir = dest / "config"
    cfg_dir.mkdir(parents=True, exist_ok=True)
    for fc, target in binaries.items():
        art = _fc_art_path(fc, target)
        if art is None:
            continue
        if (rc := _run([
            "artheia", "gen-params", str(art),
            "--out", str(cfg_dir / f"{fc}.json"),
        ])) != 0:
            return rc
    print(f"staged {cfg_dir}/<fc>.json (static params)")

    # 4. puppet apply theia::local_install — copy binaries + setcap. The binary
    #    map is passed as a Puppet hash literal via -e.
    bins = {n: str(_bazel_bin(t)) for n, t in binaries.items()}
    bins_pp = ", ".join(f"'{n}' => '{p}'" for n, p in bins.items())
    manifest = (
        "class { 'theia::local_install': "
        f"dest => '{dest}', "
        f"supervisor_src => '{_bazel_bin(supervisor_target)}', "
        f"binaries => {{ {bins_pp} }}, "
        "}"
    )
    return _run([
        "sudo", "puppet", "apply",
        f"--modulepath={PUPPET / 'modules'}",
        f"--hiera_config={PUPPET / 'hiera.yaml'}",
        "-e", manifest,
        *[a for a in args if a.startswith("-")],
    ])


# The distributed rig: one module + the full-vehicle SoftwareSpecification
# export (DemoSoftware has all machines). This is the SOLE place the Python rig
# is touched for deploy — `theia manifest` emits JSON, and `theia dist` then
# works purely from that JSON (no rig.py). Dev iteration uses `bazel build
# @rig_zonal//<host>:image` directly (rules/rig.bzl), unchanged.
_ZONAL_RIG_MODULE = "apps.manifest.zonal_rig"
_ZONAL_RIG_ATTR = "DemoSoftware"
_MANIFEST_DIR = "dist/manifest"

# machine.json CPU arch → bazel platform label (for the per-host cross-build).
_ARCH_PLATFORM = {
    "x86_64": "//rules/config:host",
    "aarch64": "//rules/config:rpi4",
}


def _emit_manifest_build_files(mdir: Path, machines: list[dict]) -> None:
    """Drop the bazel glue alongside the regenerated JSON so `theia dist` can
    reference each host's manifest files as labels and build a dist_ipk target.
    Written by `theia manifest` because it owns (the gitignored) dist/manifest/.

    Per-host BUILD exports the JSONs; the top-level BUILD declares one
    dist_ipk(name=<host>) per `target` machine."""
    for m in machines:
        (mdir / m["name"] / "BUILD.bazel").write_text(
            "# AUTO-GENERATED by `theia manifest`. exports the JSON manifests "
            "as bazel labels.\n"
            'package(default_visibility = ["//visibility:public"])\n'
            'exports_files(["machine.json", "application.json", '
            '"service.json", "execution.json", "executor.json"])\n'
        )
    targets = [m["name"] for m in machines if m.get("kind") == "target"]
    lines = [
        "# AUTO-GENERATED by `theia manifest`. One .deb per target host, packed",
        "# from that host's application.json (rules/dist_ipk.bzl). The <host>_pkg",
        "# target emits .deb (default); pass format=\"ipk\" for the opkg hatch.",
        'load("//rules:dist_ipk.bzl", "dist_pkg")',
        "",
    ]
    lines += [f'dist_pkg(name = "{h}")' for h in targets]
    (mdir / "BUILD.bazel").write_text("\n".join(lines) + "\n")


def cmd_manifest(args: list[str]) -> int:
    """THE sole rig entry for deploy: run the Python rig once and emit the JSON
    manifest set to dist/manifest/ — machines.json + per-host {machine,
    application,service,execution}.json — plus the bazel glue (`theia dist`
    consumes the JSON, never the rig). Default rig: apps.manifest.zonal_rig
    --rig DemoSoftware. Pass a module / --rig / --out to override.

    Dev iteration stays on `bazel build @rig_zonal//<host>:image` (rules/rig.bzl)
    — that path is untouched."""
    import json
    # Address-collision gate FIRST: a duplicate TIPC (type, instance) across FCs
    # silently mis-wires the runtime, so fail before emitting any manifest.
    if (rc := _check_tipc_addresses()) != 0:
        return rc
    # Rig discovery (name-INDEPENDENT, workspace-scoped): prefer an explicit
    # zonal_rig, else the workspace's plain manifest/rig.py, else the framework's
    # bundled default. Only the framework default carries the fixed _ZONAL_RIG_ATTR
    # (DemoSoftware); a discovered consuming-workspace rig lets the resolver
    # auto-rank its *Software / *Rig export (--rig omitted).
    discovered = _discover_rig_module(zonal=True) or _discover_rig_module()
    default_module = discovered or _ZONAL_RIG_MODULE
    module = next((a for a in args if not a.startswith("-")), default_module)
    rig_arg = next((args[i + 1] for i, a in enumerate(args) if a == "--rig"), None)
    out = WORKSPACE / _MANIFEST_DIR
    cmd = ["artheia", "generate-manifest", module, "--out", str(out)]
    # Pin the rig attr only for the framework default (its export is named
    # DemoSoftware); a discovered/explicit rig auto-resolves.
    if rig_arg:
        cmd += ["--rig", rig_arg]
    elif module == _ZONAL_RIG_MODULE:
        cmd += ["--rig", _ZONAL_RIG_ATTR]
    if (rc := _run(cmd)) != 0:
        return rc
    # Read back machines.json + drop the bazel glue for `theia dist`.
    machines = json.loads((out / "machines.json").read_text())["machines"]
    _emit_manifest_build_files(out, machines)
    print(f"theia manifest: {len(machines)} machines → {out}/ (+ BUILD glue)",
          file=sys.stderr)
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
    machines = json.loads(machines_json.read_text())["machines"]
    rc_final = 0
    for m in machines:
        if m.get("kind") != "target":
            continue                       # host-role (admin) — nothing to pack
        host = m["name"]
        mj = mdir / host / "machine.json"
        if not mj.is_file():
            print(f"theia dist: missing {mj}", file=sys.stderr)
            return 1
        arch = json.loads(mj.read_text())["machine"]["hardware"]["cpu"]["architecture"]
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
    return rc_final


# ── theia release — build the installable package set (.deb + .ipk) ──────────
_DIST_DEBIAN = "dist/debian"
_DIST_IPKG = "dist/ipkg"

# arch token (from --arch) → bazel platform label. host = this machine (amd64);
# rpi4 = aarch64 cross-build (needs the rpi4 C++ toolchain registered).
_RELEASE_ARCH = {
    "host": "//rules/config:host",
    "rpi4": "//rules/config:rpi4",
}

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
    """Build theia-framework as a real .deb (ROS2 /opt/ros/<distro> layout).

    Lays out, under a staged /opt/theia:
      lib/python3/site-packages/artheia/…   the artheia package (pip --target;
                                            NOT system site-packages)
      wheels/*.whl                          artheia's third-party deps, for the
                                            postinst to pip-install OFFLINE into
                                            the system (textX/Jinja2/click/PyYAML)
      rules/, toolchains/                   the bazel rules a workspace needs
                                            (@theia_framework//rules:rig.bzl)
      bin/{artheia,theia,tdb,artheia-lsp}   console shims onto the staged lib
      setup.{bash,zsh}                      `source` to put Theia on PATH+PYTHONPATH

    The deb's postinst pip-installs the bundled dep wheels into the system. Built
    with dpkg-deb (the framework is pure-Python — Architecture: all)."""
    import shutil

    pkg_root = WORKSPACE / "packaging" / "theia" / "framework"
    py = "python3.10"
    stage = out_dir / "_stage"
    if stage.exists():
        shutil.rmtree(stage)
    opt = stage / "opt" / "theia"
    site = opt / "lib" / py / "site-packages"
    site.mkdir(parents=True, exist_ok=True)
    (opt / "bin").mkdir(parents=True, exist_ok=True)
    (opt / "wheels").mkdir(parents=True, exist_ok=True)

    # artheia → /opt/theia/lib/.../site-packages (no deps; they go to the system).
    if (rc := _run([sys.executable, "-m", "pip", "install", "--no-deps",
                    "--target", str(site), str(WORKSPACE / "artheia")])) != 0:
        return rc
    # artheia's deps as wheels for the offline system install (postinst).
    if (rc := _run([sys.executable, "-m", "pip", "download",
                    "--dest", str(opt / "wheels"),
                    "textX>=4.0", "Jinja2>=3.1", "click>=8.1", "PyYAML>=6.0",
                    "nanopb>=0.4.9"])) != 0:
        # Non-fatal: postinst falls back to PyPI if the wheels aren't bundled.
        print("theia release: framework dep download failed — deb postinst will "
              "fetch from PyPI instead.", file=sys.stderr)

    # bazel rules + toolchains the downstream workspace's MODULE.bazel needs.
    for d in ("rules", "toolchains"):
        if (WORKSPACE / d).is_dir():
            shutil.copytree(WORKSPACE / d, opt / d, dirs_exist_ok=True)
    # Make /opt/theia a valid Bazel repo so a workspace can
    # `local_repository(name="theia_framework", path="/opt/theia")` and load
    # @theia_framework//rules:rig.bzl. A bare REPO.bazel + a root BUILD is enough.
    (opt / "REPO.bazel").write_text('repo(name = "theia_framework")\n')
    if not (opt / "BUILD.bazel").exists():
        (opt / "BUILD.bazel").write_text(
            '# theia_framework repo root.\n'
            'package(default_visibility = ["//visibility:public"])\n')
    # theia.py itself (the workspace lifecycle driver) + setup scripts.
    shutil.copy2(WORKSPACE / "theia.py", opt / "theia.py")
    for s in ("setup.bash", "setup.zsh"):
        shutil.copy2(pkg_root / s, opt / s)

    # Console shims: run the staged package with the staged site-packages on path.
    shims = {
        "artheia": "from artheia.cli import main",
        "artheia-lsp": "from artheia.lsp.server import main",
    }
    for name, imp in shims.items():
        shim = opt / "bin" / name
        shim.write_text(
            "#!/bin/sh\n"
            'D="$(cd "$(dirname "$0")/.." && pwd)"\n'
            f'exec python3 -c '
            f'"import sys; sys.path.insert(0, \\"$D/lib/{py}/site-packages\\"); '
            f'{imp}; main()" "$@"\n')
        shim.chmod(0o755)
    # theia + tdb shims (theia.py at /opt/theia/theia.py; tdb from runtime deb).
    (opt / "bin" / "theia").write_text(
        '#!/bin/sh\nexec python3 "$(dirname "$0")/../theia.py" "$@"\n')
    (opt / "bin" / "theia").chmod(0o755)

    # control + postinst.
    ctrl = stage / "DEBIAN"
    ctrl.mkdir(parents=True, exist_ok=True)
    installed_kb = int(_du_kb(opt))
    (ctrl / "control").write_text(
        "Package: theia-framework\n"
        f"Version: {version}\n"
        "Architecture: all\n"
        "Section: devel\n"
        "Priority: optional\n"
        "Maintainer: Theia <theia@robofortis.com>\n"
        f"Installed-Size: {installed_kb}\n"
        "Depends: python3, python3-pip\n"
        "Description: Theia framework — artheia DSL/codegen + bazel rules + CLIs\n")
    shutil.copy2(pkg_root / "postinst", ctrl / "postinst")
    (ctrl / "postinst").chmod(0o755)

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
    for i, a in enumerate(args):
        if a == "--arch" and i + 1 < len(args):
            archs = [x.strip() for x in args[i + 1].split(",") if x.strip()]
    for a in archs:
        if a not in _RELEASE_ARCH:
            print(f"theia release: unknown arch '{a}' "
                  f"(known: {', '.join(_RELEASE_ARCH)})", file=sys.stderr)
            return 2

    deb_dir = WORKSPACE / _DIST_DEBIAN
    ipk_dir = WORKSPACE / _DIST_IPKG
    deb_dir.mkdir(parents=True, exist_ok=True)

    python_only = "--python-only" in args
    # .deb is the default + primary (Theia is always Debian-derived). --ipk is the
    # opt-in hatch that ALSO emits the embedded/opkg .ipk under dist/ipkg/.
    want_ipk = "--ipk" in args

    # ── Step 1: framework — artheia + deps + rules → a real .deb (/opt/theia,
    #    ROS2-style setup.bash). Arch-independent (Architecture: all). ──────────
    fw_out = deb_dir / "theia-framework"
    fw_out.mkdir(parents=True, exist_ok=True)
    if (rc := _build_framework_deb(fw_out)) != 0:
        print("theia release: framework .deb build failed.", file=sys.stderr)
        return rc

    # ── Step 4 (python part): rf harness wheel (minus _selftest, per its
    #    pyproject find.exclude). Arch-independent. ────────────────────────────
    rf_out = deb_dir / "theia-rf"
    rf_out.mkdir(parents=True, exist_ok=True)
    if (rc := _run([sys.executable, "-m", "pip", "wheel",
                    str(WORKSPACE / "testing"), "--no-deps",
                    "-w", str(rf_out)])) != 0:
        print("theia release: rf wheel build failed.", file=sys.stderr)
        return rc

    if python_only:
        print(f"theia release: python wheels → {fw_out}, {rf_out}",
              file=sys.stderr)
        return 0

    # ── Steps 2+3: runtime + services .deb (+ .ipk with --ipk) via bazel. ─────
    for arch in archs:
        platform = _RELEASE_ARCH[arch]
        deb_targets = [d for d, _ in _RELEASE_BAZEL_PKGS]
        # .deb (default — per arch, emits *_amd64.deb / *_arm64.deb).
        if (rc := _run(["bazel", "build", *deb_targets,
                        f"--platforms={platform}"])) != 0:
            return rc
        # .ipk hatch (embedded/opkg) — only with --ipk.
        if want_ipk:
            ipk_targets = [i for _, i in _RELEASE_BAZEL_PKGS if i]
            if (rc := _run(["bazel", "build", *ipk_targets,
                            f"--platforms={platform}"])) != 0:
                return rc

    # ── Step 4 (collect): copy bazel-bin outputs into dist/. ─────────────────
    bin_root = WORKSPACE / "bazel-bin" / "packaging" / "theia"
    n_deb = n_ipk = 0
    for f in bin_root.glob("*.deb"):
        pkg = f.name.split("_")[0]          # theia-runtime_0.1.0_amd64.deb → theia-runtime
        dst = deb_dir / pkg
        dst.mkdir(parents=True, exist_ok=True)
        shutil.copy2(f, dst / f.name)
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


def cmd_init(args: list[str]) -> int:
    """Scaffold the CURRENT directory as a Theia consuming workspace.

    The catkin-`catkin init` / ROS-`colcon` analogue: turns an empty repo into
    a workspace that builds apps against a SIBLING Theia source checkout (or an
    installed /opt/theia prefix later). Run it from your workspace root after
    `source /path/to/theia/setup.sh`:

        cd ~/repo/launch-box/gataway_ws
        source ../theia/setup.sh           # exports THEIA_ROOT
        theia init [--name <ws>]           # bare workspace (supervisor + your apps)
        theia init --with-services         # + the ARA services (com/log/per/sm/ucm/shwa)

    --with-services bootstraps the workspace with the platform services: it links
    system/services and emits a rig built on the framework's ServicesSoftware, so
    a bare `theia start` brings the full service tree up under the supervisor.

    It creates, in the CWD (never overwriting an existing file):
      - system/system.art   — the workspace aggregator. Imports the Theia
        clusters (services / supervisor) you'll deploy, plus a stub you fill in
        by hand (link system/<yourthing> + add its cluster). You then `theia
        manifest` against it.
      - manifest/rig.py      — a one-machine, one-app rig stub importing the
        generated sidecars.
      - .theia               — records THEIA_ROOT (the source it's bound to).

    Theia itself is NOT vendored: system/runtime + the platform/ wrappers are
    SYMLINKS into $THEIA_ROOT, so a Theia bump is a re-source, not a re-copy."""
    if "-h" in args or "--help" in args:
        print(cmd_init.__doc__, file=sys.stderr)
        return 0

    theia_root = os.environ.get("THEIA_ROOT")
    if not theia_root:
        print("theia init: THEIA_ROOT is unset — `source /path/to/theia/setup.sh` "
              "first so the workspace knows where the Theia source lives.",
              file=sys.stderr)
        return 2
    theia_root = Path(theia_root).resolve()
    if not (theia_root / "system" / "system.art").is_file():
        print(f"theia init: THEIA_ROOT={theia_root} doesn't look like a Theia "
              "checkout (no system/system.art).", file=sys.stderr)
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

    # system/ aggregator + the runtime link (Theia's .art import root).
    _link("system/runtime", theia_root / "system" / "runtime")
    _link("system/supervisor", theia_root / "system" / "supervisor")
    # The runtime .art package: a service/supervisor .art imports `platform.runtime.*`
    # (ChildControlIf, TraceControlPush, LogLevelPush), which the resolver maps to
    # platform/runtime/package.art. Link it to the sibling Theia's runtime spec.
    _link("platform/runtime/package.art",
          theia_root / "platform" / "runtime" / "system" / "runtime" / "package.art")
    # --with-services: link the framework's ARA service FCs so `cluster Services`
    # resolves + the rig can import services.manifest.{service,executor}.
    if with_services:
        _link("system/services", theia_root / "system" / "services")
    # The workspace's OWN empty app package (no compositions yet). gen-manifest
    # walks `cluster Applications` here — empty → an empty app manifest +
    # executor sidecar, which the rig imports as-is. WRITE before the symlink so
    # the target dir exists.
    _write("apps/system/apps/package.art", _INIT_APPS_PACKAGE_ART)
    _write("apps/system/apps/component.art", _INIT_APPS_COMPONENT_ART)
    _write("apps/__init__.py", "")
    _write("apps/manifest/__init__.py", "")
    # system/apps → THIS workspace's own app package (an IN-workspace target, not
    # the framework). The user adds compositions there; until then the workspace
    # still inits/builds/runs a bare supervisor.
    _link("system/apps", ws / "apps" / "system" / "apps")

    sys_art = (_INIT_SYSTEM_ART_SERVICES if with_services else _INIT_SYSTEM_ART)
    rig_py = (_INIT_RIG_PY_SERVICES if with_services else _INIT_RIG_PY)
    _write("system/system.art", sys_art.replace("@NAME@", name))
    _write("manifest/__init__.py", "")
    _write("manifest/rig.py", rig_py.replace("@NAME@", name))
    _write(".theia", f"THEIA_ROOT={theia_root}\nname={name}\n")
    _write("README.md", _INIT_README.replace("@NAME@", name)
                                   .replace("@THEIA_ROOT@", str(theia_root)))

    flavour = "services workspace" if with_services else "empty workspace"
    print(f"\ntheia init: scaffolded '{name}' ({flavour}) against {theia_root}",
          file=sys.stderr)
    for c in created:
        print(f"  + {c}", file=sys.stderr)
    extra = ("\n  (the ARA services com/log/per/sm/ucm/shwa come up under the "
             "supervisor)" if with_services else "")
    print("\nVerify the toolchain before adding apps:\n"
          "  artheia gen-manifest apps/system/apps/component.art "
          "apps/manifest/app.py\n"
          f"  theia manifest && theia install && theia start{extra}\n"
          "Then add a composition to apps/system/apps/component.art and "
          "`artheia gen-app` it.", file=sys.stderr)
    return 0


_INIT_SYSTEM_ART = '''\
// @NAME@ — Theia consuming-workspace aggregator.
//
// Imports the Theia supervisor + THIS workspace's own app package (resolved
// through the system/ symlinks). `theia manifest` walks this file.
//
// EMPTY-workspace shape: just the supervisor + an (empty) Applications cluster.
// Add your app by declaring a `composition` in apps/system/apps/component.art
// and `cluster Applications { composition <Yours> <id> }` — it flows here via
// the import below, no edit needed.

package system

import system.supervisor.*   // the OTP-style supervisor (the runtime fabric)
import system.apps.*         // THIS workspace's app package (system/apps → apps/system/apps)

// --- forward-decl: the clusters this workspace deploys --------------------
cluster Applications { }     // empty until you add a composition in apps/

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
//   4. `artheia gen-app --kind fc apps/system/apps/component.art --out apps
//       --composition MyApp` to emit the C++.

package system.apps

cluster Applications { }
'''

_INIT_RIG_PY = '''\
"""@NAME@ deploy rig — one machine, a bare supervisor + this workspace's apps.

EMPTY-workspace rig: imports the generated (initially empty) app manifest +
its executor sidecar and runs a one-machine supervisor. Verify the toolchain
NOW (theia manifest / install / start) — it works with zero apps. As you add
compositions to apps/system/apps/component.art and regenerate the manifest
(`artheia gen-manifest apps/system/apps/component.art apps/manifest/app.py`),
the app_sup children + processes appear here automatically.

See $THEIA_ROOT/docs/skills/theia/references/deployment.md.
"""
from __future__ import annotations

from typing import cast

from artheia.manifest import (
    ApplicationManifest, MachineManifest, SupervisorNode, SwComponent,
    VehicleIdentity, Rig,
)
from artheia.manifest.rig import SoftwareSpecification, Append, SetTransformTypes
from artheia.manifest.machine import HardwareResource, CpuResource, CpuArchitecture

# The generated app manifest (empty until you add apps). gen-manifest writes
# apps/manifest/app.py with APPLICATIONS_PROCESSES / APPLICATIONS_SHORTS +
# an executor.py sidecar (app_sup with the app children). Both are empty for a
# fresh workspace — import defensively so `theia init` → `theia manifest`
# works before the first gen-manifest run.
try:
    from apps.manifest.app import (
        APPLICATIONS_PROCESSES as _APP_PROCESSES,
        APPLICATIONS_SHORTS as _APP_SHORTS,
    )
except Exception:               # not generated yet → empty workspace
    _APP_PROCESSES, _APP_SHORTS = [], []

Host = MachineManifest(
    name="@NAME@",
    hardware=HardwareResource(cpu=CpuResource(architecture=CpuArchitecture.X86_64)),
)

# The supervisor binary — the runtime fabric every machine runs. `theia install`
# builds + stages it (as <machine>/supervisor, not bin/). The framework provides
# the target //platform/supervisor/main:supervisor.
_SUPERVISOR = SwComponent(
    name="supervisor",
    bazel_target="//platform/supervisor/main:supervisor",
    owner="platform",
    art_node="system.supervisor/Supervisor",
    bazel_buildable=True,
)

# root → app_sup → <your apps> (empty list = bare supervisor, no app children).
SUPERVISORS = [
    SupervisorNode(name="root", children=["app_sup"]),
    SupervisorNode(name="app_sup", children=list(_APP_SHORTS)),
]

# `*Software` is auto-ranked first by the rig resolver (--rig optional).
Software = SoftwareSpecification(
    vehicle=VehicleIdentity(name="@NAME@", make="theia", model="workspace"),
    machines=cast(set[SetTransformTypes], {Append(Host)}),
    applications=cast(set[SetTransformTypes], {
        Append(ApplicationManifest(name="app", host_machine=Host.name,
                                   components=[_SUPERVISOR])),
    }),
    execution_manifests=cast(set[SetTransformTypes],
                             {Append(p) for p in _APP_PROCESSES}),
    supervisors=cast(set[SetTransformTypes], {Append(s) for s in SUPERVISORS}),
)

# Materialized rig (for callers that isinstance-check on Rig).
WorkspaceRig: Rig = Software.to_rig()
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
"""@NAME@ deploy rig — one machine: the ARA services + this workspace's apps.

WITH-SERVICES rig: built on the framework's ServicesSoftware (the full FC set +
the OTP supervisor tree from services.manifest.executor), with this workspace's
apps Appended into app_sup. `theia install` builds the FC binaries (com/log/per/
sm/ucm/shwa) + the supervisor; `theia start` runs the whole service tree.

As you add compositions to apps/system/apps/component.art and regenerate
(`artheia gen-manifest apps/system/apps/component.art apps/manifest/app.py`),
the app leaves appear under app_sup automatically.

See $THEIA_ROOT/docs/skills/theia/references/deployment.md.
"""
from __future__ import annotations

import dataclasses
from typing import cast

from artheia.manifest import (
    ApplicationManifest, MachineManifest, SwComponent, VehicleIdentity, Rig,
)
from artheia.manifest.rig import SoftwareSpecification, Append, SetTransformTypes
from artheia.manifest.machine import HardwareResource, CpuResource, CpuArchitecture

# The framework's ARA services: the FC components/processes + the OTP supervisor
# tree (root → ar_sup → core_sup → … with app_sup). ServicesSoftware is a full
# SoftwareSpecification we squash our apps onto.
from services.manifest.service import (
    ServicesSoftware as _ServicesSoftware,   # private alias: keep `Software` the
    SERVICES_COMPONENTS as _FC_COMPONENTS,   # ONLY public *Software export so the
    SERVICES_PROCESSES as _FC_PROCESSES,     # rig resolver picks it unambiguously
    SUPERVISORS as _PLATFORM_SUPERVISORS,
)

# The supervisor binary (the runtime fabric). SERVICES_COMPONENTS is the 6 FCs
# only; theia install also needs the supervisor SwComponent to build + stage it.
_SUPERVISOR = SwComponent(
    name="supervisor",
    bazel_target="//platform/supervisor/main:supervisor",
    owner="platform",
    art_node="system.supervisor/Supervisor",
    bazel_buildable=True,
)

# This workspace's generated app manifest (empty until you add apps).
try:
    from apps.manifest.app import (
        APPLICATIONS_PROCESSES as _APP_PROCESSES,
        APPLICATIONS_COMPONENTS as _APP_COMPONENTS,
        APPLICATIONS_SHORTS as _APP_SHORTS,
    )
except Exception:               # not generated yet → services-only
    _APP_PROCESSES, _APP_COMPONENTS, _APP_SHORTS = [], [], []

Host = MachineManifest(
    name="@NAME@",
    hardware=HardwareResource(cpu=CpuResource(architecture=CpuArchitecture.X86_64)),
)

# The platform supervisor tree with app_sup carrying THIS workspace's apps.
_supervisors = [
    dataclasses.replace(s, children=list(_APP_SHORTS))
    if s.name == "app_sup" else s
    for s in _PLATFORM_SUPERVISORS
]

# Layer the workspace deltas (one machine, the FCs bound to it, the apps) onto
# ServicesSoftware. `Software` is the only public *Software export → the rig
# resolver picks it (the imported services spec is private as _ServicesSoftware).
Software = _ServicesSoftware.squash(SoftwareSpecification(
    vehicle=VehicleIdentity(name="@NAME@", make="theia", model="workspace"),
    machines=cast(set[SetTransformTypes], {Append(Host)}),
    applications=cast(set[SetTransformTypes], {
        # platform_app on this host: the FC components + the supervisor binary.
        Append(ApplicationManifest(name="platform_app", host_machine=Host.name,
                                   components=list(_FC_COMPONENTS) + [_SUPERVISOR])),
        Append(ApplicationManifest(name="app", host_machine=Host.name,
                                   components=list(_APP_COMPONENTS))),
    }),
    execution_manifests=cast(set[SetTransformTypes],
                             {Append(p) for p in (_FC_PROCESSES + _APP_PROCESSES)}),
    supervisors=cast(set[SetTransformTypes], {Append(s) for s in _supervisors}),
))

WorkspaceRig: Rig = Software.to_rig()
'''

_INIT_README = '''\
# @NAME@ — Theia consuming workspace

Built against a SIBLING Theia source checkout (THEIA_ROOT=@THEIA_ROOT@),
not vendored. system/runtime + system/services + system/supervisor are
symlinks into it.

```sh
source @THEIA_ROOT@/setup.sh     # exports THEIA_ROOT, puts `theia` on PATH
theia init                       # (already run — scaffolded this dir)
# link your app/gateway spec, import it in system/system.art, then:
theia manifest
theia install
theia start && tdb ps
```

When Theia ships as a deb, swap the sibling source for /opt/theia
(THEIA_ROOT=/opt/theia) — the symlinks + rig stay the same.
'''


COMMANDS = {
    "init":        (cmd_init,        "scaffold the CWD as a Theia consuming workspace (sibling-source)"),
    "rig":         (cmd_rig,         "docker compose {up|down} the deploy stack"),
    "provision":   (cmd_provision,   "puppet apply — Phase 1 (os pkgs + .ipk)"),
    "orchestrate": (cmd_orchestrate, "puppet apply — Phase 2 remote (app rollout)"),
    "install":     (cmd_install,     "build + puppet-populate install/<machine>/ (local host)"),
    "stage-local": (cmd_install,     "alias for `install` (back-compat)"),
    "start":       (cmd_start,       "run the staged supervisor from install/<machine>/ (detached + pidfile)"),
    "stop":        (cmd_stop,        "stop the supervisor started by `theia start` (graceful)"),
    "manifest":    (cmd_manifest,    "rig.py → dist/manifest/*.json (sole rig entry for deploy)"),
    "dist":        (cmd_dist,        "per-host .ipk from dist/manifest/ JSON (no rig.py)"),
    "release":     (cmd_release,     "build the installable package set (.deb+.ipk) → dist/debian + dist/ipkg"),
    "compdb":      (cmd_compdb,      "regen compile_commands.json from bazel (clangd)"),
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
    os.chdir(WORKSPACE)  # bazel / compose / puppet paths are workspace-relative
    return fn(rest)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

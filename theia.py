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
    theia dist            bazel .ipk bundles + manifests → dist/manifest/<machine>/
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

WORKSPACE = Path(__file__).resolve().parent
COMPOSE = WORKSPACE / "deploy" / "docker-compose.yml"
PUPPET = WORKSPACE / "deploy" / "puppet"


def _run(argv: list[str]) -> int:
    """Run a command from the workspace root, streaming output; return rc."""
    print(f"$ {' '.join(argv)}", file=sys.stderr)
    try:
        return subprocess.call(argv)
    except FileNotFoundError as e:
        print(f"theia: {e}", file=sys.stderr)
        return 127
    except KeyboardInterrupt:
        return 130


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


# Central single-machine local stage: the bin-name (executor.json start_cmd
# leaf, bin/<name>) -> bazel target the binary is built from. The supervisor is
# handled separately (lands at <dest>/supervisor, not bin/).
_LOCAL_BINARIES = {
    "log":  "//services/log/main:log",
    "sm":   "//services/sm/main:sm",
    "per":  "//services/per/main:per",
    "ucm":  "//services/ucm/main:ucm",
    "shwa": "//services/shwa/main:shwa",
    "p1":   "//demo/Demo3WayP1/main:demo",
    "p2":   "//demo/Demo3WayP2/main:demo",
    "p3":   "//demo/Demo3WayP3/main:demo",
}
_LOCAL_SUPERVISOR = "//platform/supervisor/main:supervisor"


def _bazel_bin(target: str) -> Path:
    """//pkg/dir:name -> WORKSPACE/bazel-bin/pkg/dir/name."""
    pkg, name = target.lstrip("/").split(":", 1)
    return WORKSPACE / "bazel-bin" / pkg / name


def _fc_art_path(fc: str, target: str):
    """The .art the gen-params emitter reads for an FC. Services FCs live at the
    canonical symlink path system/services/<fc>/package.art; demo apps
    (//demo/...) at system/demo/package.art. Returns a Path or None if absent
    (an FC with no .art simply gets no params file)."""
    if target.startswith("//demo/"):
        cand = WORKSPACE / "system" / "demo" / "package.art"
    else:
        cand = WORKSPACE / "system" / "services" / fc / "package.art"
    return cand if cand.exists() else None


def cmd_install(args: list[str]) -> int:
    """LOCAL install: build + populate $WORKSPACE/install/<machine>/ via puppet.

    The dev inner-loop counterpart of the remote .ipk deploy, and the inherited
    home of demo/stage_local.sh. bazel builds the binaries (its job) + artheia
    emits executor.json + per-FC params; then `puppet apply theia::local_install`
    copies them into install/<machine>/ and applies the SAME setcap contract
    (theia::postinstall) a real deploy uses — "bazel builds, puppet orchestrates
    the host". Default machine: central. Pass a machine name to override.

    (`theia stage-local` is a back-compat alias for this verb.)"""
    import json

    machine = next((a for a in args if not a.startswith("-")), "central")
    rig = {"central": "CentralRig"}.get(machine, "CentralRig")
    dest = WORKSPACE / "install" / machine

    # 1. bazel build — the supervisor + every child binary.
    targets = [_LOCAL_SUPERVISOR, *_LOCAL_BINARIES.values()]
    if (rc := _run(["bazel", "build", *targets])) != 0:
        return rc

    # 2. executor.json — the supervisor tree for this machine.
    dest.mkdir(parents=True, exist_ok=True)
    if (rc := _run([
        "artheia", "executor", "emit", "demo.manifest.rig",
        "--rig", rig, "--out", str(dest / "executor.json"),
    ])) != 0:
        return rc

    # 2b. Per-FC static params JSON — config/<fc>.json, one per FC that declares
    #     a params {} block. Read once at boot by the runtime config singleton
    #     (init_config(<fc>) resolves $THEIA_CONFIG_DIR/<fc>.json; the supervisor
    #     sets THEIA_CONFIG_DIR=config in the child env, see executor emit). A
    #     params-less FC emits an empty {nodes:{}} (harmless; lookups default).
    cfg_dir = dest / "config"
    cfg_dir.mkdir(parents=True, exist_ok=True)
    for fc, target in _LOCAL_BINARIES.items():
        art = _fc_art_path(fc, target)
        if art is None:
            continue
        if (rc := _run([
            "artheia", "gen-params", str(art),
            "--out", str(cfg_dir / f"{fc}.json"),
        ])) != 0:
            return rc
    print(f"staged {cfg_dir}/<fc>.json (static params)")

    # 2c. The four AUTOSAR manifest kinds (machine/application/service/
    #     execution.json) per machine → install/manifest/<machine>/. Single-
    #     machine local rig (demo.manifest.rig) — no --rig attr needed.
    if (rc := _run([
        "artheia", "generate-manifest", "demo.manifest.rig",
        "--out", str(WORKSPACE / "install" / "manifest"),
    ])) != 0:
        return rc

    # 3. puppet apply theia::local_install — copy binaries + setcap. The binary
    #    map is passed as a Puppet hash literal via -e.
    bins = {n: str(_bazel_bin(t)) for n, t in _LOCAL_BINARIES.items()}
    bins_pp = ", ".join(f"'{n}' => '{p}'" for n, p in bins.items())
    manifest = (
        "class { 'theia::local_install': "
        f"dest => '{dest}', "
        f"supervisor_src => '{_bazel_bin(_LOCAL_SUPERVISOR)}', "
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
# export (DemoSoftware has all machines; CentralRig/ComputeRig are the
# materialized per-machine rigs). Everything `theia dist` needs derives from it.
_ZONAL_RIG_MODULE = "demo.manifest.zonal_rig"
_ZONAL_RIG_ATTR = "DemoSoftware"
_ZONAL_RIG_REPO = "rig_zonal"   # the @rig_zonal bazel extension repo


def _zonal_image_targets() -> list[str]:
    """Derive the per-machine `:image` bazel labels from the rig itself (not a
    hardcoded list). Ask `artheia rig-deps` for the machines + their buildable
    components; a machine with at least one buildable component gets an
    @rig_zonal//<machine>:image target. Host-role machines with no buildable
    components (e.g. admin_host) are skipped."""
    import json
    proc = subprocess.run(
        ["artheia", "rig-deps", _ZONAL_RIG_MODULE, "--rig", _ZONAL_RIG_ATTR],
        capture_output=True, text=True,
    )
    if proc.returncode != 0:
        print(proc.stderr, file=sys.stderr)
        return []
    rig = json.loads(proc.stdout)
    flat = rig.get("flat_components", [])
    buildable = {
        c["machine"] for c in flat if c.get("bazel_buildable")
    }
    return [
        f"@{_ZONAL_RIG_REPO}//{m['name']}:image"
        for m in rig["machines"]
        if m["name"] in buildable
    ]


def cmd_dist(args: list[str]) -> int:
    """bazel build the per-machine .ipk deploy bundles for the DISTRIBUTED
    deploy AND emit the per-machine AUTOSAR manifest set (machine/application/
    service/execution.json) to dist/manifest/<machine>/.

    Both the .ipk targets and the manifests derive from the SAME rig spec —
    `demo.manifest.zonal_rig --rig DemoSoftware` (the full-vehicle spec). The
    image labels are NOT hardcoded: `artheia rig-deps` enumerates the machines
    and which have buildable components. Pass explicit bazel labels to narrow.

    (The single-machine local host bundle is `theia install`.)"""
    targets = args or _zonal_image_targets()
    if not targets:
        print("theia dist: no buildable machine images derived from "
              f"{_ZONAL_RIG_MODULE} --rig {_ZONAL_RIG_ATTR}", file=sys.stderr)
        return 1
    if (rc := _run(["bazel", "build", *targets])) != 0:
        return rc
    # The four AUTOSAR manifest kinds per machine → dist/manifest/<machine>/,
    # from the SAME rig+attr the image targets derive from.
    return _run([
        "artheia", "generate-manifest", _ZONAL_RIG_MODULE,
        "--rig", _ZONAL_RIG_ATTR,
        "--out", str(WORKSPACE / "dist" / "manifest"),
    ])


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
    "//demo/...",
    "//gateway/libs/...",
]


def cmd_compdb(args: list[str]) -> int:
    """Regenerate compile_commands.json from Bazel (for clangd).

    Runs `bazel aquery mnemonic(CppCompile, <targets>)` and writes the
    {file, arguments, directory} entries to compile_commands.json at the
    workspace root. Pass target patterns to narrow the scope; default
    covers services/platform/demo/gateway. Pass `--config=<x>` (or any
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
        # Tolerate packages that fail to load/analyze (e.g. the retired
        # //platform/supervisor:* bindings still referenced by the CMake/grpc
        # edge in services/com + services/log:all_srcs on psp-retirement).
        # aquery still emits valid jsonproto for everything that DID analyze;
        # we index that and report the skips. Drop this once those consumers
        # are repointed off the retired supervisor proto targets.
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


COMMANDS = {
    "rig":         (cmd_rig,         "docker compose {up|down} the deploy stack"),
    "provision":   (cmd_provision,   "puppet apply — Phase 1 (os pkgs + .ipk)"),
    "orchestrate": (cmd_orchestrate, "puppet apply — Phase 2 remote (app rollout)"),
    "install":     (cmd_install,     "build + puppet-populate install/<machine>/ (local host)"),
    "stage-local": (cmd_install,     "alias for `install` (back-compat)"),
    "dist":        (cmd_dist,        "per-machine .ipk bundles + manifests (dist/manifest/)"),
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
    os.chdir(WORKSPACE)  # bazel / compose / puppet paths are workspace-relative
    return fn(rest)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

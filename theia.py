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


# TIPC address-collision gate. The .art aggregators that, followed recursively,
# union the WIDEST cross-FC node set for the deploy — so a duplicate (type,
# instance) across DIFFERENT FCs (e.g. com's ComDaemon vs per's PerManager both
# claiming 0x80010008) is caught here, before manifests/dist are built. TIPC
# routes purely by address, so a collision is a silent runtime mis-wire.
_ADDRESS_CHECK_ARTS = (
    "system/services/cluster.art",   # all platform FCs (com + per + …)
    "system/system.art",             # demo + supervisor + gateway
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
    "p4":   "//demo/Demo3WayP4/main:demo",   # gen_statem test FSM
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

    # 0. Address-collision gate — fail BEFORE building/staging if two nodes
    #    share a TIPC (type, instance) anywhere in the deployed FC set.
    if (rc := _check_tipc_addresses()) != 0:
        return rc

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
# export (DemoSoftware has all machines). This is the SOLE place the Python rig
# is touched for deploy — `theia manifest` emits JSON, and `theia dist` then
# works purely from that JSON (no rig.py). Dev iteration uses `bazel build
# @rig_zonal//<host>:image` directly (rules/rig.bzl), unchanged.
_ZONAL_RIG_MODULE = "demo.manifest.zonal_rig"
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
        "# AUTO-GENERATED by `theia manifest`. One .ipk per target host, packed",
        "# from that host's application.json (rules/dist_ipk.bzl).",
        'load("//rules:dist_ipk.bzl", "dist_ipk")',
        "",
    ]
    lines += [f'dist_ipk(name = "{h}")' for h in targets]
    (mdir / "BUILD.bazel").write_text("\n".join(lines) + "\n")


def cmd_manifest(args: list[str]) -> int:
    """THE sole rig entry for deploy: run the Python rig once and emit the JSON
    manifest set to dist/manifest/ — machines.json + per-host {machine,
    application,service,execution}.json — plus the bazel glue (`theia dist`
    consumes the JSON, never the rig). Default rig: demo.manifest.zonal_rig
    --rig DemoSoftware. Pass a module / --rig / --out to override.

    Dev iteration stays on `bazel build @rig_zonal//<host>:image` (rules/rig.bzl)
    — that path is untouched."""
    import json
    # Address-collision gate FIRST: a duplicate TIPC (type, instance) across FCs
    # silently mis-wires the runtime, so fail before emitting any manifest.
    if (rc := _check_tipc_addresses()) != 0:
        return rc
    module = next((a for a in args if not a.startswith("-")), _ZONAL_RIG_MODULE)
    rig_attr = _ZONAL_RIG_ATTR if module == _ZONAL_RIG_MODULE else None
    out = WORKSPACE / _MANIFEST_DIR
    cmd = ["artheia", "generate-manifest", module, "--out", str(out)]
    if rig_attr:
        cmd += ["--rig", rig_attr]
    if (rc := _run(cmd)) != 0:
        return rc
    # Read back machines.json + drop the bazel glue for `theia dist`.
    machines = json.loads((out / "machines.json").read_text())["machines"]
    _emit_manifest_build_files(out, machines)
    print(f"theia manifest: {len(machines)} machines → {out}/ (+ BUILD glue)",
          file=sys.stderr)
    return 0


def cmd_dist(args: list[str]) -> int:
    """Build the per-host .ipk deploy bundles from the JSON manifests — NO rig.py.

    Arg: the machines.json path (default dist/manifest/machines.json). Reads it
    for the host list, then for each `target` host reads <host>/machine.json for
    the arch → bazel platform and builds //dist/manifest:<host>_ipk (rules/
    dist_ipk.bzl packs from <host>/application.json). One bazel invocation per
    host so each cross-compiles to its own arch.

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
        # //dist/manifest:<host>_ipk, cross-compiled for the host's arch.
        if (rc := _run([
            "bazel", "build",
            f"//{_MANIFEST_DIR}:{host}_ipk",
            f"--platforms={platform}",
        ])) != 0:
            rc_final = rc
    return rc_final


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


COMMANDS = {
    "rig":         (cmd_rig,         "docker compose {up|down} the deploy stack"),
    "provision":   (cmd_provision,   "puppet apply — Phase 1 (os pkgs + .ipk)"),
    "orchestrate": (cmd_orchestrate, "puppet apply — Phase 2 remote (app rollout)"),
    "install":     (cmd_install,     "build + puppet-populate install/<machine>/ (local host)"),
    "stage-local": (cmd_install,     "alias for `install` (back-compat)"),
    "manifest":    (cmd_manifest,    "rig.py → dist/manifest/*.json (sole rig entry for deploy)"),
    "dist":        (cmd_dist,        "per-host .ipk from dist/manifest/ JSON (no rig.py)"),
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

#!/usr/bin/env python3
"""pack_ipk.py — build a per-host .ipk/.deb from a serialized execution.json.

The DIST (no-python, no-rig-eval) half of the manifest engine: the `dist_pkg`
bazel rule hands this script a host's execution.json + machine.json (from a
COMMITTED serialized dir — `dist/manifest/<host>/`) and the FULL set of buildable
binaries (a fixed filegroup — bazel deps must be known at analysis time, but the
targets live INSIDE the JSON read at exec time). This script parses the JSON,
picks the binaries this host actually wants, and packs them into an opkg/dpkg
.ipk.

It reads the SAME JSON shapes `artheia serialize-manifest` emits — but from
committed files, so dist is reproducible WITHOUT running the Python rig:

    machine.json    {"name","arch":"x86_64"|"aarch64",...}         → the arch tag
    execution.json  {"processes":[{"name","executable","start_cmd",...}]}
                    → name = the deploy basename; executable = the bazel target.

An .ipk is an ar(1) archive: debian-binary + control.tar.gz + data.tar.gz —
byte-compatible with what rules/opkg.bzl (pkg_opkg) emits, so the JSON-driven
`theia dist` and the dev `bazel build @rig//:image` produce the same package.

Binaries land at /opt/theia/bin/<name> (the process name, matching the executor
start_cmd `bin/<name>` under THEIA_ROOT_DIR=/opt/theia). The bin's bazel target
is matched to a candidate from the filegroup by the target's package path (a
suffix of the bazel-out path) — NOT basename — because the demo p1/p2/p3/p4
binaries are ALL the cc_binary `:apps`, distinguished only by package.

Usage (invoked by the dist_pkg rule, not by hand):
    pack_ipk.py --exec execution.json --machine machine.json \\
                --out <host>.ipk --bin <path> [--bin <path> ...]
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tarfile
import tempfile


# CPU arch (machine.json["arch"], e.g. x86_64/aarch64) → package Architecture
# tag. .deb wants amd64/arm64; .ipk (opkg) wants x86_64/aarch64.
_DEB_ARCH = {"x86_64": "amd64", "k8": "amd64", "aarch64": "arm64", "arm64": "arm64"}
_IPK_ARCH = {"amd64": "x86_64", "arm64": "aarch64", "x86_64": "x86_64",
             "aarch64": "aarch64", "k8": "x86_64"}


def _arch(machine_json: str, fmt: str = "deb") -> str:
    """The per-host Architecture tag, from machine.json's flat `arch` field
    (the serialize-manifest shape). .deb → amd64/arm64; .ipk → x86_64/aarch64."""
    with open(machine_json) as f:
        cpu = json.load(f)["arch"]
    table = _IPK_ARCH if fmt == "ipk" else _DEB_ARCH
    return table.get(cpu, cpu)


def _du_kb(path: str) -> int:
    """KiB of the staged data tree (dpkg Installed-Size convention)."""
    total = 0
    for root, _dirs, files in os.walk(path):
        for nm in files:
            try:
                total += os.path.getsize(os.path.join(root, nm))
            except OSError:
                pass
    return (total + 1023) // 1024


def _target_pkg_path(bazel_target: str) -> str:
    """`//apps/Demo3WayP1/main:apps` → `apps/Demo3WayP1/main/apps` — the path
    segment that uniquely identifies the binary (so the demo p1/p2/p3/p4, which
    are ALL the cc_binary `:apps`, are distinguished by package, not basename)."""
    label = bazel_target.lstrip("/")
    pkg, _, name = label.partition(":")
    return f"{pkg}/{name}" if name else pkg


def _wanted(exec_json: str) -> dict[str, tuple[str, str]]:
    """process name → (/opt/theia/bin/<name>, target_pkg_path) for every
    process on this host, read from execution.json's `processes` list."""
    with open(exec_json) as f:
        doc = json.load(f)
    out: dict[str, tuple[str, str]] = {}
    for p in doc.get("processes", []):
        out[p["name"]] = (
            "/opt/theia/bin/" + p["name"],
            _target_pkg_path(p["executable"]),
        )
    # The supervisor is ALWAYS staged at bin/supervisor — it's the runtime that
    # boots the executor.json tree (the PG allocator + watchdog), not a process
    # row in execution.json. theia dist adds its label to the binaries filegroup;
    # stage it here so the .deb is actually bootable (`theia start` runs it).
    out["supervisor"] = (
        "/opt/theia/bin/supervisor",
        _target_pkg_path("//platform/supervisor/main:supervisor"),
    )
    return out


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exec", dest="execution", required=True,
                    help="path to the host's execution.json (process list)")
    ap.add_argument("--machine", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--package", required=True)
    ap.add_argument("--version", default="1.0.0")
    ap.add_argument("--bin", action="append", default=[],
                    help="path to a candidate binary (the fixed filegroup)")
    ap.add_argument("--lib", action="append", default=[],
                    help="shared lib to bundle at /opt/theia/lib/<basename>")
    ap.add_argument("--format", choices=["deb", "ipk"], default="deb",
                    help="archive format (same ar layout; deb adds Installed-Size "
                         "+ amd64 arch, ipk is opkg-lean + x86_64 arch).")
    args = ap.parse_args(argv)

    wanted = _wanted(args.execution)     # name → (dest, target_pkg_path)
    arch = _arch(args.machine, args.format)

    # Match each wanted process to a candidate binary by the target's package
    # path (a suffix of the binary's bazel-out path). A candidate not wanted by
    # this host is simply skipped (over-declared dep). name → resolved src path.
    resolved: dict[str, str] = {}
    for name, (_dest, pkg_path) in wanted.items():
        hit = next((p for p in args.bin if p.endswith("/" + pkg_path)), None)
        if hit:
            resolved[name] = hit
    missing = [n for n in wanted if n not in resolved]
    if missing:
        sys.stderr.write(
            f"pack_ipk: {args.package}: processes in execution.json have no "
            f"matching binary in the filegroup: {missing}\n")
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        data = os.path.join(tmp, "data")
        ctrl = os.path.join(tmp, "ctrl")
        os.makedirs(data)
        os.makedirs(ctrl)

        # data tree: copy each wanted binary to its dest, mode 0755 (executable;
        # bazel-out is read-only 0555).
        for name in sorted(wanted):
            dest = wanted[name][0]     # /opt/theia/bin/<name>
            dst = data + dest          # → data/opt/theia/bin/<name>
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(resolved[name], "rb") as r, open(dst, "wb") as w:
                w.write(r.read())
            os.chmod(dst, 0o755)

        # shared libs → /opt/theia/lib/<basename>. run-supervisor.sh puts
        # /opt/theia/lib on LD_LIBRARY_PATH so the children resolve them.
        for lib in args.lib:
            dst = os.path.join(data, "opt/theia/lib", os.path.basename(lib))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(lib, "rb") as r, open(dst, "wb") as w:
                w.write(r.read())
            os.chmod(dst, 0o755)

        # control file. The archive is .deb-format either way (dpkg installs it);
        # the .deb default adds Installed-Size (a dpkg/apt nicety), the --ipk
        # hatch keeps the leaner opkg control.
        control = (
            f"Package: {args.package}\n"
            f"Version: {args.version}\n"
            f"Architecture: {arch}\n"
            "Section: apps\n"
            "Priority: optional\n"
            "Maintainer: Theia <theia@example.com>\n"
        )
        if args.format == "deb":
            installed_kb = _du_kb(data)
            control += f"Installed-Size: {installed_kb}\n"
        control += f"Description: Deploy bundle for {args.package}\n"
        with open(os.path.join(ctrl, "control"), "w") as f:
            f.write(control)

        # data.tar.gz + control.tar.gz (deterministic: sorted, fixed mtime/owner)
        def _tar(src_dir: str, out_tgz: str) -> None:
            def _reset(ti: tarfile.TarInfo) -> tarfile.TarInfo:
                ti.uid = ti.gid = 0
                ti.uname = ti.gname = "root"
                ti.mtime = 0
                return ti
            with tarfile.open(out_tgz, "w:gz") as t:
                for root, dirs, files in os.walk(src_dir):
                    for nm in sorted(dirs) + sorted(files):
                        full = os.path.join(root, nm)
                        arc = "./" + os.path.relpath(full, src_dir)
                        t.add(full, arcname=arc, recursive=False, filter=_reset)

        data_tgz = os.path.join(tmp, "data.tar.gz")
        ctrl_tgz = os.path.join(tmp, "control.tar.gz")
        _tar(data, data_tgz)
        _tar(ctrl, ctrl_tgz)

        debbin = os.path.join(tmp, "debian-binary")
        with open(debbin, "w") as f:
            f.write("2.0\n")

        # ar the three members into the .ipk.
        out_abs = os.path.abspath(args.out)
        if os.path.exists(out_abs):
            os.remove(out_abs)
        subprocess.run(
            ["ar", "cr", out_abs, debbin, ctrl_tgz, data_tgz],
            check=True,
        )
    sys.stderr.write(f"Built: {os.path.basename(args.out)} "
                     f"({arch}, {len(wanted)} binaries)\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

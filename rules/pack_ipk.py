#!/usr/bin/env python3
"""pack_ipk.py — build a per-host .ipk from a parsed application.json.

The deploy half of the JSON pivot (docs/tasks/TODO/manifest-json-pivot.md): the
`dist_ipk` bazel rule hands this script a host's application.json + machine.json
and the FULL set of buildable binaries (a fixed filegroup — bazel deps must be
known at analysis time, but the targets live INSIDE the JSON read at exec time).
This script parses the JSON, picks the binaries this host actually wants, and
packs them into an opkg/dpkg-compatible .ipk.

An .ipk is an ar(1) archive: debian-binary + control.tar.gz + data.tar.gz —
byte-compatible with what rules/opkg.bzl (pkg_opkg) emits, so the JSON-driven
`theia dist` and the dev `bazel build @rig//:image` produce the same package.

Binaries land at /opt/theia/bin/<name> (the derived dest — application.json
carries `name` + `bazel_target`, not an explicit path; the convention matches
the executor start_cmd `bin/<name>` under THEIA_ROOT_DIR=/opt/theia).

Usage (invoked by the dist_ipk rule, not by hand):
    pack_ipk.py --app application.json --machine machine.json \\
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


def _arch(machine_json: str, fmt: str = "deb") -> str:
    """The per-host Architecture tag, from machine.json's declared CPU arch.
    .deb → amd64 / arm64; .ipk → x86_64 / aarch64."""
    with open(machine_json) as f:
        cpu = json.load(f)["machine"]["hardware"]["cpu"]["architecture"]
    if fmt == "ipk":
        return {"amd64": "x86_64", "arm64": "aarch64"}.get(cpu, cpu)
    return {"x86_64": "amd64", "aarch64": "arm64"}.get(cpu, cpu)


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
    """`//apps/Demo3WayP1/main:demo` → `apps/Demo3WayP1/main/demo` — the path
    segment that uniquely identifies the binary (so the demo p1/p2/p3, which are
    ALL named `demo`, are distinguished by package, not basename)."""
    label = bazel_target.lstrip("/")
    pkg, _, name = label.partition(":")
    return f"{pkg}/{name}" if name else pkg


def _wanted(app_json: str) -> dict[str, tuple[str, str]]:
    """component name → (/opt/theia/bin/<name>, target_pkg_path) for every
    buildable component on this host."""
    with open(app_json) as f:
        doc = json.load(f)
    out: dict[str, tuple[str, str]] = {}
    for app in doc.get("applications", []):
        for c in app.get("components", []):
            if c.get("bazel_buildable"):
                out[c["name"]] = (
                    "/opt/theia/bin/" + c["name"],
                    _target_pkg_path(c["bazel_target"]),
                )
    return out


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True)
    ap.add_argument("--machine", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--package", required=True)
    ap.add_argument("--version", default="1.0.0")
    ap.add_argument("--bin", action="append", default=[],
                    help="path to a candidate binary (the fixed filegroup)")
    ap.add_argument("--lib", action="append", default=[],
                    help="shared lib to bundle at /opt/theia/lib/<basename> "
                         "(e.g. libetcd-cpp-api.so for per)")
    ap.add_argument("--format", choices=["deb", "ipk"], default="deb",
                    help="archive format (same ar layout; deb adds Installed-Size "
                         "+ amd64 arch, ipk is opkg-lean + x86_64 arch).")
    args = ap.parse_args(argv)

    wanted = _wanted(args.app)           # name → (dest, target_pkg_path)
    arch = _arch(args.machine, args.format)

    # Match each wanted component to a candidate binary by the target's package
    # path (a suffix of the binary's bazel-out path) — NOT basename, because the
    # demo p1/p2/p3 binaries are all named `demo`. A candidate not wanted by
    # this host is simply skipped (over-declared dep). name → resolved src path.
    resolved: dict[str, str] = {}
    for name, (_dest, pkg_path) in wanted.items():
        hit = next((p for p in args.bin if p.endswith("/" + pkg_path)), None)
        if hit:
            resolved[name] = hit
    missing = [n for n in wanted if n not in resolved]
    if missing:
        sys.stderr.write(
            f"pack_ipk: {args.package}: components in application.json have no "
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

        # shared libs → /opt/theia/lib/<basename> (e.g. libetcd-cpp-api.so for
        # per). run-supervisor.sh puts /opt/theia/lib on LD_LIBRARY_PATH so the
        # children resolve them.
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

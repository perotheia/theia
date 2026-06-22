#!/usr/bin/env python3
"""gen_ansible.py — turn a serialized rig manifest + its inventory into the
Ansible group_vars the generic orchestrate playbook consumes.

The deploy flow is manifest-driven and rig-generic:

    demo/manifest/<rig>/rig.py        the rig (logical: machines, processes, tree)
      │  artheia serialize-manifest
      ▼
    demo/build/<rig>/                 machines.json + <machine>/{machine,execution,
      │                               executor,service,application}.json
      │  this script  ⊕  demo/inventory/<rig>/hosts.ini   (machine → ssh host/user)
      ▼
    demo/inventory/<rig>/group_vars/<machine>.yml   per-machine deploy facts
      │   (group_vars sits BESIDE hosts.ini → Ansible auto-loads it by group name,
      │    which is the machine name)
      ▼
    ansible-playbook -i demo/inventory/<rig>/hosts.ini demo/deploy/orchestrate.yml
      ▼
    each box: build the machine's arch binaries, stage them, run the supervisor

What we emit per machine (read by orchestrate.yml):
  machine        : the manifest machine name (== inventory `machine=`)
  arch           : machine.json arch (aarch64 / x86_64) — the bazel --platforms
  binaries       : [{label, bin}] from execution.json processes ON THIS MACHINE
                   (label = bazel target to build; bin = start_cmd basename to
                   stage at /opt/theia/bin/<bin>)
  build_dir      : demo/build/<rig>/<machine>  (executor.json + config/ live here)

The machine→host mapping is the ONE thing the manifest can't carry (it's a
deploy/network fact, not logical), so it lives in demo/inventory/<rig>/hosts.ini
where each inventory GROUP is named after a manifest machine. group_vars files
are named after the GROUP (== machine), so every host in group <machine> picks
up that machine's deploy vars automatically.

Usage:  demo/deploy/gen_ansible.py <rig>
        (re-run after `artheia serialize-manifest manifest.<rig>.rig --out
         demo/build/<rig>`)
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

# arch token in machine.json → bazel --platforms label
_ARCH_PLATFORM = {
    "aarch64": "//rules/config:rpi4",
    "arm64":   "//rules/config:rpi4",
    "x86_64":  "//rules/config:host",
    "amd64":   "//rules/config:host",
}

_REPO = Path(__file__).resolve().parents[2]   # demo/deploy/ → repo root


def _machines(build_dir: Path) -> list[str]:
    return json.loads((build_dir / "machines.json").read_text())["machines"]


def _binaries_for(build_dir: Path, machine: str) -> list[dict]:
    """The processes execution.json places ON `machine` → [{label, bin}].
    label = bazel target to build; bin = the staged /opt/theia/bin/<name>."""
    execu = json.loads((build_dir / machine / "execution.json").read_text())
    seen: dict[str, dict] = {}
    for p in execu.get("processes", []):
        if p.get("machine") != machine:
            continue
        label = p.get("executable", "")
        # start_cmd is "bin/<name>"; the staged binary basename.
        bin_name = Path(p.get("start_cmd", "")).name or p.get("name", "")
        if label and bin_name and bin_name not in seen:
            seen[bin_name] = {"label": label, "bin": bin_name}
    # The supervisor itself is implicit (not a manifest process) — every machine
    # runs it. Stage it first.
    out = [{"label": "//platform/supervisor/main:supervisor", "bin": "supervisor"}]
    out += sorted(seen.values(), key=lambda d: d["bin"])
    return out


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: gen_ansible.py <rig>", file=sys.stderr)
        return 2
    rig = argv[1]
    build_dir = _REPO / "demo" / "build" / rig
    if not (build_dir / "machines.json").is_file():
        print(f"error: {build_dir}/machines.json not found — run first:\n"
              f"  artheia serialize-manifest manifest.{rig}.rig --attr RIG "
              f"--out demo/build/{rig}", file=sys.stderr)
        return 1

    # group_vars beside the rig's inventory so Ansible auto-loads by group name.
    inv_dir = _REPO / "demo" / "inventory" / rig
    if not (inv_dir / "hosts.ini").is_file():
        print(f"warning: {inv_dir}/hosts.ini not found — create the host map for "
              f"rig '{rig}' (machine→ssh). Emitting group_vars anyway.",
              file=sys.stderr)
    gv_dir = inv_dir / "group_vars"
    gv_dir.mkdir(parents=True, exist_ok=True)

    emitted = []
    for machine in _machines(build_dir):
        mj = json.loads((build_dir / machine / "machine.json").read_text())
        arch = mj.get("arch", "x86_64")
        platform = _ARCH_PLATFORM.get(arch, "//rules/config:host")
        binaries = _binaries_for(build_dir, machine)
        # group_vars file is named after the inventory GROUP, which is the
        # machine name (see demo/inventory/<rig>.ini). Every host in that group
        # inherits these.
        gv = {
            "machine": machine,
            "arch": arch,
            "bazel_platform": platform,
            # the controller dir holding executor.json + config/ for this machine
            "build_dir": str(build_dir / machine),
            "binaries": binaries,
        }
        out = gv_dir / f"{machine}.yml"
        # Plain-YAML by hand (no PyYAML dep): the structure is simple + flat.
        lines = [f"# AUTO-GENERATED by demo/deploy/gen_ansible.py from "
                 f"demo/build/{rig}/{machine}/ — DO NOT EDIT.",
                 f"machine: {machine}",
                 f"arch: {arch}",
                 f"bazel_platform: \"{platform}\"",
                 f"build_dir: \"{gv['build_dir']}\"",
                 "binaries:"]
        for b in binaries:
            lines.append(f"  - {{ label: \"{b['label']}\", bin: \"{b['bin']}\" }}")
        out.write_text("\n".join(lines) + "\n")
        emitted.append((machine, arch, len(binaries)))

    for m, a, n in emitted:
        print(f"  {rig}/{m}: arch={a}, {n} binaries → "
              f"demo/inventory/{rig}/group_vars/{m}.yml")
    print(f"gen_ansible: {len(emitted)} machine(s) for rig '{rig}'.")
    print(f"  next: ansible-playbook -i demo/inventory/{rig}/hosts.ini "
          f"demo/deploy/orchestrate.yml")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

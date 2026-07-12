"""targets.bzl — the SINGLE declarative registry of Theia cross-compile targets.

The standardized answer to "add a new board". Before this, each target (rpi4, then
the Ubuntu-24 distro key, then the Jetson focal) was rediscovered: a hand-written
toolchain-<x>.cmake, a hardcoded sysroot path, an arch→platform line in theia.py,
and scattered select()s. One new board = a multi-file archaeology dig + glibc
gotchas hit at link time.

Now: ONE entry here describes a target fully, and every layer reads it —
  - the bazel platform + the cmake cross-toolchain (sysroot, gcc prefix),
  - theia.py's arch-token → {platform, sysroot, deb ABI key} map,
  - colony's runtime-plane S3 key (<ver>-<abi_key>).

A target is a (cpu, libc/distro) PAIR — NOT just a cpu — because aarch64-bookworm
(glibc 2.36) and aarch64-focal (glibc 2.31) are DIFFERENT ABIs that need different
sysroots and produce non-interchangeable binaries. This is the lesson the Jetson
taught: the rpi4 sysroot's binaries reference GLIBC_2.34 (the libpthread→libc merge)
+ GLIBC_2.38 and won't run on focal.

To add a target: append one TARGETS entry. The sysroot is host data (gitignored,
bootstrapped per third_party/sysroot/setup_<name>.sh); this registry only names
WHERE it is and HOW to drive the toolchain at it.
"""

# Each target:
#   cpu          @platforms//cpu value (x86_64 | aarch64)
#   gcc_prefix   the cross-gcc triple prefix ("" = native host gcc; else e.g.
#                "aarch64-linux-gnu-"). Native (host arch == cpu) needs no prefix.
#   sysroot      path (relative to repo root) to the target sysroot; "" = host root
#                (native build, no --sysroot). gitignored host data.
#   abi_key      the S3 runtime-plane / deb ABI qualifier — what distinguishes two
#                same-cpu builds (bookworm vs focal vs ubuntu24). "" = arch-agnostic
#                (the host-x86 dev build). This is the <ver>-<abi_key> S3 suffix.
#   libc_min     the minimum glibc the target provides (a sanity gate: a built
#                binary must not reference a HIGHER GLIBC_ symbol than this).
#   deb_arch     dpkg arch for the .deb (amd64 | arm64).
TARGETS = {
    # the workspace host — native x86, dev/CI box (NOT a shipped distro per se).
    "host": {
        "cpu": "x86_64", "gcc_prefix": "", "sysroot": "",
        "abi_key": "", "libc_min": "", "deb_arch": "amd64",
    },
    # Raspberry Pi 4 — aarch64, Debian 12 bookworm (glibc 2.36). The original
    # cross target; sysroot bootstrapped by third_party/sysroot/setup_rpi4.sh.
    "rpi4": {
        "cpu": "aarch64", "gcc_prefix": "aarch64-linux-gnu-",
        "sysroot": "third_party/sysroot/rpi4",
        "abi_key": "bookworm-arm64", "libc_min": "2.36", "deb_arch": "arm64",
    },
    # NVIDIA Jetson AGX — aarch64, L4T R35.2.1 (Ubuntu 20.04 focal, glibc 2.31).
    # SEPARATE ABI from rpi4: focal lacks the pthread-in-libc-2.34 merge, so
    # rpi4-sysroot binaries (GLIBC_2.34/2.38) won't load. Its sysroot carries a
    # focal-gcc-9 + a from-source grpc 1.51/abseil/protobuf 3.21 closure.
    "jetson": {
        "cpu": "aarch64", "gcc_prefix": "aarch64-linux-gnu-",
        "sysroot": "third_party/sysroot/jetson",
        "abi_key": "focal-arm64", "libc_min": "2.31", "deb_arch": "arm64",
    },
    # NVIDIA Jetson Orin (Nano) — aarch64, L4T r36 (Ubuntu 22.04 jammy,
    # glibc 2.35). THIRD aarch64 ABI: bookworm binaries reference GLIBC_2.36
    # (won't load on 2.35); focal binaries would load but jammy is its own
    # distro key. CROSS-compiled from the dev box like rpi4 (an OTA-updated
    # mobile target is never a build host): jammy sysroot bootstrapped by
    # third_party/sysroot/setup_orin.sh, which also CROSS-builds the static
    # grpc-1.51/protobuf-3.21 closure into <sysroot>/usr/local (jammy apt ships
    # 1.30/3.12 — too old). The build host being jammy itself makes gcc-11
    # aarch64 a perfect glibc/GLIBCXX match. TIPC on the board: the tegra
    # kernel ships CONFIG_TIPC=n — tipc.ko is built out-of-tree with the
    # tipc_devmap shim (net_device::tipc_ptr → RCU side table).
    "orin": {
        "cpu": "aarch64", "gcc_prefix": "aarch64-linux-gnu-",
        "sysroot": "third_party/sysroot/orin",
        "abi_key": "jammy-arm64", "libc_min": "2.35", "deb_arch": "arm64",
    },
}

def target_for_arch(arch):
    """arch token (the user-facing --arch / --distro selector) → the target dict.
    Accepts a target NAME directly (rpi4/jetson/host) or the bare cpu for the
    legacy default (aarch64→rpi4, x86_64→host)."""
    if arch in TARGETS:
        return TARGETS[arch]
    for name, t in TARGETS.items():  # legacy: bare cpu → first matching target
        if t["cpu"] == arch:
            return t
    return None

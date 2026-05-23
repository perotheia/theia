#!/usr/bin/env bash
# third_party/sysroot/setup_rpi4.sh — bootstrap an aarch64 Debian bookworm
# sysroot for cross-compiling Theia C++ binaries to Raspberry Pi 4
# (running Raspberry Pi OS 12 / Debian 12).
#
# What ends up in the sysroot:
#   - minbase Debian bookworm rootfs (~80 MB)
#   - lib*-dev for: libyaml-cpp, libprotobuf, libgrpc++, libgrpc, libabsl
#   - matching .so + headers for cross-link
#
# Total size: ~450 MB. Gitignored; re-run this script after blowing
# away the workspace or to refresh deps.
#
# Prerequisites (apt install):
#   - debootstrap
#   - qemu-user-static (for the --second-stage chroot to run aarch64
#     scripts under emulation)
#   - gcc-aarch64-linux-gnu  (the compiler; see step 1 of
#     docs/tasks/BACKLOG/cross-compile-rpi4.md)
#
# After this script: cross-compile with
#
#   SR=$(pwd)/third_party/sysroot/rpi4
#   aarch64-linux-gnu-g++ --sysroot="$SR" \
#       -I"$SR/usr/include" \
#       -L"$SR/usr/lib/aarch64-linux-gnu" \
#       -Wl,-rpath-link="$SR/usr/lib/aarch64-linux-gnu" \
#       -o out src.cpp -lyaml-cpp
#
# Functional-test it on the host (no Pi needed):
#
#   qemu-aarch64-static -L "$SR" ./out
#
# Idempotent: re-running on an existing sysroot is a no-op (debootstrap
# refuses to bootstrap onto a non-empty target). To refresh, delete the
# sysroot dir first.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="$SCRIPT_DIR/rpi4"
SUITE="bookworm"
MIRROR="http://deb.debian.org/debian"

# The library set the supervisor + services-com link against. Match
# names from `dpkg -l | grep -E 'libgrpc|libyaml|libprotobuf|libabsl'`
# on a clean Debian bookworm install.
PACKAGES=(
    libyaml-cpp-dev
    libprotobuf-dev
    libgrpc++-dev
    libgrpc-dev
    libabsl-dev
)

err() { echo "setup_rpi4: $*" >&2; }

# --- preconditions ---------------------------------------------------

for tool in debootstrap qemu-aarch64-static aarch64-linux-gnu-g++; do
    if ! command -v "$tool" >/dev/null; then
        err "missing required tool: $tool"
        err "see docs/tasks/BACKLOG/cross-compile-rpi4.md for install steps"
        exit 1
    fi
done

if [[ -d "$TARGET" && -n "$(ls -A "$TARGET" 2>/dev/null)" ]]; then
    err "sysroot already exists at $TARGET (size: $(du -sh "$TARGET" | cut -f1))"
    err "delete it first to re-bootstrap"
    exit 0
fi

# --- first stage: download + unpack .debs ---------------------------
# `--foreign` defers package-config scripts that need to run as
# aarch64 to the second stage. minbase keeps the rootfs ~80 MB.

mkdir -p "$TARGET"

echo "==> first stage: debootstrap --arch=arm64 --foreign $SUITE"
sudo debootstrap \
    --arch=arm64 \
    --foreign \
    --variant=minbase \
    --include="$(IFS=,; echo "${PACKAGES[*]}")" \
    "$SUITE" \
    "$TARGET" \
    "$MIRROR"

# --- prep for second stage ------------------------------------------
# binfmt + qemu-user-static let `chroot "$TARGET" /...` run aarch64
# binaries on this x86 host. binfmt is registered system-wide by the
# qemu-user-static package install; we just need a copy of the
# emulator inside the sysroot for the chroot to find it.

sudo cp /usr/bin/qemu-aarch64-static "$TARGET/usr/bin/"

# --- second stage: run package-config under emulation ---------------

echo "==> second stage: chroot debootstrap --second-stage"
sudo chroot "$TARGET" /debootstrap/debootstrap --second-stage

# --- sanity check ---------------------------------------------------

echo
echo "==> sysroot ready at $TARGET ($(sudo du -sh "$TARGET" | cut -f1))"

missing=()
for lib in libyaml-cpp.so.0.7 libprotobuf.so libgrpc++.so libabsl_strings.so; do
    if ! sudo find "$TARGET/usr/lib/aarch64-linux-gnu" -name "${lib}*" \
        -quit -print 2>/dev/null | grep -q .; then
        missing+=("$lib")
    fi
done
if (( ${#missing[@]} )); then
    err "WARN: expected libs not found: ${missing[*]}"
    exit 2
fi

echo "==> all expected libs present"
echo
echo "Next: cross-compile a smoke binary, e.g.:"
echo "  SR=\"$TARGET\""
echo "  aarch64-linux-gnu-g++ --sysroot=\"\$SR\" -I\"\$SR/usr/include\" \\"
echo "      -L\"\$SR/usr/lib/aarch64-linux-gnu\" -lyaml-cpp src.cpp -o out"
echo "  qemu-aarch64-static -L \"\$SR\" ./out"

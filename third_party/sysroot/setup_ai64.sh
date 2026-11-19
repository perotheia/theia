#!/usr/bin/env bash
# third_party/sysroot/setup_ai64.sh — bootstrap an aarch64 Debian TRIXIE sysroot
# for cross-compiling Theia to the BeagleBone AI-64 (TDA4VM, Debian 13, glibc
# 2.41).
#
# WHY ITS OWN SYSROOT (fourth aarch64 ABI): the board runs trixie, and unlike
# the rpi4 target (bookworm sysroot, forward-compatible binaries) this one is
# built AGAINST trixie, so the trixie-only -dev packages the services need
# (nanopb 0.4.9, iceoryx 2.0.x for ara::rds, abseil 20240722) come straight
# from the board's own archive. The link needs a libstdc++ >= GLIBCXX_3.4.33
# on the build host: Ubuntu 24.04's libstdc++6-arm64-cross (gcc 14 runtime)
# has it, whichever aarch64-linux-gnu-g++ (11 or 13) drives the link. The
# Ubuntu 22.04 cross runtime does NOT (see setup_rpi4.sh's header) — build
# this target on a 24.04+ host.
#
# trixie keeps protobuf 3.21.12 / grpc 1.51.1 — the SAME releases as bookworm —
# so the staged host-x86 codegen toolset (codegen-bookworm-x86, see
# setup_codegen_x86.sh) serves this target unchanged.
#
# The sysroot lands at third_party/sysroot/ai64. That path may be a SYMLINK to
# a bigger disk (ln -s /bigdata/…/sysroot/ai64 third_party/sysroot/ai64) — or
# set THEIA_AI64_SYSROOT=<dir> for both this script and the bazel build.
#
# Prerequisites: debootstrap, qemu-user-static (binfmt registered),
# gcc/g++-aarch64-linux-gnu, curl. Idempotent per phase.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="${THEIA_AI64_SYSROOT:-$SCRIPT_DIR/ai64}"
SUITE="trixie"
MIRROR="http://deb.debian.org/debian"
CG_DIR="$SCRIPT_DIR/../codegen-bookworm-x86"
MA="aarch64-linux-gnu"

# The -dev set. setup_rpi4.sh's list, plus what the trixie archive carries
# that the bookworm sysroot stages by hand or leaves out.
PACKAGES=(
    libyaml-cpp-dev
    libprotobuf-dev
    libgrpc++-dev
    libgrpc-dev
    libabsl-dev
    # grpc's CMake config checks <sysroot>/usr/bin/{protoc,grpc_*_plugin}
    # (see setup_rpi4.sh); the etcd cmake of services/per trips on it.
    protobuf-compiler-grpc
    protobuf-compiler
    # the static grpc closure com/per link (-lre2 -lcares -lz)
    libre2-dev
    libc-ares-dev
    zlib1g-dev
    # etcd-cpp-apiv3 -core (services/per)
    libboost-system-dev
    libboost-thread-dev
    libboost-random-dev
    libssl-dev
    libcpprest-dev     # etcd's optional async client (docs/HOST-PASCAL.md list)
    # Per-FC native libs
    libnftables-dev    # services/fw
    libbpf-dev         # services/idsm (opt-in eBPF backend)
    libelf-dev
    libmnl-dev         # services/nm
    libnl-3-dev
    libnl-route-3-dev
    libacl1-dev        # ara::rds (iceoryx posix ACLs)
    # nanopb runtime from the archive (pb.h + libprotobuf-nanopb.a) — NOT the
    # generator package `nanopb` (the generator runs on the host).
    libnanopb-dev
    # ara::rds transport (services/rds) — trixie ships iceoryx 2.0.x.
    libiceoryx-binding-c-dev
    libiceoryx-posh-dev
    libiceoryx-hoofs-dev
)

err() { echo "setup_ai64: $*" >&2; }

for tool in debootstrap qemu-aarch64-static aarch64-linux-gnu-g++ curl; do
    command -v "$tool" >/dev/null || { err "missing required tool: $tool"; exit 1; }
done

# ── Phase 1: rootfs ─────────────────────────────────────────────────────────
if [ ! -e "$TARGET/etc/os-release" ]; then
    mkdir -p "$TARGET"
    KEYRING=/usr/share/keyrings/debian-archive-keyring.gpg
    if [ ! -e "$KEYRING" ]; then
        # An Ubuntu host has no Debian archive key installed. Take it from the
        # host distro's own (apt-verified) debian-archive-keyring package
        # without installing it.
        kr="$(mktemp -d)"
        ( cd "$kr" && apt-get download debian-archive-keyring >/dev/null \
            && dpkg-deb -x debian-archive-keyring_*.deb x )
        KEYRING="$kr/x/usr/share/keyrings/debian-archive-keyring.gpg"
        [ -e "$KEYRING" ] || { err "no Debian archive keyring (apt install debian-archive-keyring)"; exit 1; }
    fi
    echo "==> debootstrap --arch=arm64 $SUITE → $TARGET"
    sudo debootstrap --arch=arm64 --variant=minbase --keyring="$KEYRING" \
        --include=ca-certificates "$SUITE" "$TARGET" "$MIRROR"
fi

# ── Phase 2: the -dev library set (chroot apt under qemu binfmt) ────────────
if [ ! -e "$TARGET/usr/include/yaml-cpp/yaml.h" ]; then
    # binfmt without the F flag needs the emulator inside the chroot.
    [ -e "$TARGET/usr/bin/qemu-aarch64-static" ] \
        || sudo cp /usr/bin/qemu-aarch64-static "$TARGET/usr/bin/" 2>/dev/null || true
    sudo chroot "$TARGET" apt-get update -qq
    # Report (do not fail on) packages the suite does not carry.
    want=() ; gaps=()
    for p in "${PACKAGES[@]}"; do
        if sudo chroot "$TARGET" apt-cache show "$p" >/dev/null 2>&1; then
            want+=("$p")
        else
            gaps+=("$p")
        fi
    done
    (( ${#gaps[@]} )) && err "NOT in $SUITE (skipped): ${gaps[*]}"
    sudo chroot "$TARGET" env DEBIAN_FRONTEND=noninteractive \
        apt-get install -y -qq --no-install-recommends "${want[@]}"
    sudo chroot "$TARGET" apt-get clean
fi

# ── Phase 3: layout fix-ups ─────────────────────────────────────────────────
# <prefix>/include for CMake package configs that bake "<prefix>/include"
# (see setup_rpi4.sh).
[ -e "$TARGET/include" ] || sudo ln -sfn usr/include "$TARGET/include"

# nanopb: trixie's libnanopb-dev installs pb.h under /usr/include/nanopb/;
# the generated *.pb.h say `#include <pb.h>`. Flat symlinks keep ONE include
# layout across the sysroots (rpi4 stages them flat).
if [ -e "$TARGET/usr/include/nanopb/pb.h" ] && [ ! -e "$TARGET/usr/include/pb.h" ]; then
    for h in pb.h pb_common.h pb_encode.h pb_decode.h; do
        sudo ln -sfn "nanopb/$h" "$TARGET/usr/include/$h"
    done
fi

# ABSOLUTE symlinks inside the sysroot (/etc/alternatives/…, /lib/…) point at
# the BUILD HOST's root when read from outside a chroot; a linker following
# one picks a host (x86) file or nothing. Rewrite the ones under the library
# dirs to relative links.
sudo find "$TARGET/usr/lib" "$TARGET/usr/include" -type l -lname '/*' -print0 2>/dev/null \
  | while IFS= read -r -d '' l; do
        tgt="$(readlink "$l")"
        [ -e "$TARGET$tgt" ] || continue
        sudo ln -sfn "$(realpath -m --relative-to="$(dirname "$l")" "$TARGET$tgt")" "$l"
    done

# ── Phase 4: host-x86 codegen toolset + the plugin at the sysroot path ──────
"$SCRIPT_DIR/setup_codegen_x86.sh" "$TARGET"
# etcd's CMakeLists EXECUTES gRPC::grpc_cpp_plugin from the sysroot path, so it
# must be the host-x86 binary there (see setup_rpi4.sh); the others are only
# existence-checked.
if ! file -L "$TARGET/usr/bin/grpc_cpp_plugin" | grep -q x86-64; then
    sudo rm -f "$TARGET/usr/bin/grpc_cpp_plugin"
    sudo cp "$CG_DIR/bin/grpc_cpp_plugin" "$TARGET/usr/bin/grpc_cpp_plugin"
fi

# ── sanity ──────────────────────────────────────────────────────────────────
echo "==> sysroot ready at $TARGET ($(sudo du -sh "$TARGET/" | cut -f1))"
missing=()
for lib in libyaml-cpp.so libprotobuf.a libgrpc++.a libabsl_strings.a \
           libprotobuf-nanopb.a libnftables.so libssl.so; do
    ls "$TARGET/usr/lib/$MA/$lib" >/dev/null 2>&1 || missing+=("$lib")
done
if (( ${#missing[@]} )); then
    err "expected libs not found: ${missing[*]}"
    exit 2
fi
echo "==> all expected libs present (glibc $(ls "$TARGET/usr/lib/$MA/" | grep -oE '^libc\.so\.[0-9]+' | head -1))"

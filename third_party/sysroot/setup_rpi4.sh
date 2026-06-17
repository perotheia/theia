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
#
# services/per static-links etcd-cpp-apiv3 (//third_party:etcd_cpp, a foreign_cc
# cmake build). Its CMakeLists find_package()s Boost (system/thread/random) +
# cpprestsdk + OpenSSL — so the cross-build needs THOSE -dev packages in the
# sysroot too, or `cmake -DCMAKE_TOOLCHAIN_FILE=…` fails at find_package(Boost).
PACKAGES=(
    libyaml-cpp-dev
    libprotobuf-dev
    libgrpc++-dev
    libgrpc-dev
    libabsl-dev
    # etcd-cpp-apiv3 (services/per) static-link deps — its CMakeLists
    # find_package()s these; the grpc-chain (re2/c-ares) comes with libgrpc-dev:
    libboost-system-dev
    libboost-thread-dev
    libboost-random-dev
    libcpprest-dev
    libssl-dev
    # Per-FC native libs (the Linux-mapping FCs link these directly):
    libnftables-dev    # services/fw  — nftables ruleset (libnftables.h)
    libbpf-dev         # services/idsm — eBPF loader (bpf/libbpf.h)
    libelf-dev         #               — libbpf's ELF dep
    libmnl-dev         # services/nm  — minimal netlink
    libnl-3-dev        # services/nm  — libnl link/addr
    libnl-route-3-dev
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

# --- prefix-compat symlinks ------------------------------------------
# Debian puts everything under /usr (/usr/include, /usr/lib). Some CMake
# package configs (gRPC, cpprestsdk — pulled in by etcd-cpp-apiv3) bake
# INTERFACE_INCLUDE_DIRECTORIES as "<prefix>/include"; with CMAKE_SYSROOT set
# that resolves to "<sysroot>/include", which doesn't exist on a /usr-only
# Debian layout → "Imported target includes non-existent path". Symlink the
# top-level include (lib is already symlinked by debootstrap) so those configs
# resolve. Harmless for the Bazel cc_toolchain (it uses -I"$SR/usr/include").
sudo ln -sfn usr/include "$TARGET/include"
# --- nanopb (not reliably in the Debian arm64 archive) --------------
# The Theia runtime/protos #include <pb.h> and link libprotobuf-nanopb.a.
# nanopb is 3 tiny .c files — cross-compile them into the sysroot's lib +
# stage the headers, so the aarch64 cc_toolchain finds them.
echo "==> staging nanopb (cross-compile pb_*.c → aarch64)"
NANOPB_VER="${NANOPB_VER:-0.4.7}"
np="$(mktemp -d)"
curl -fsSL "https://github.com/nanopb/nanopb/archive/refs/tags/${NANOPB_VER}.tar.gz" \
    | tar xz -C "$np"
nps="$np/nanopb-${NANOPB_VER}"
( cd "$nps" && for c in pb_common pb_encode pb_decode; do
      aarch64-linux-gnu-gcc -c -O2 -fPIC -I. "$c.c" -o "$c.o"
  done
  aarch64-linux-gnu-ar rcs libprotobuf-nanopb.a pb_common.o pb_encode.o pb_decode.o )
sudo cp "$nps"/pb.h "$nps"/pb_common.h "$nps"/pb_encode.h "$nps"/pb_decode.h \
    "$TARGET/usr/include/"
sudo cp "$nps/libprotobuf-nanopb.a" "$TARGET/usr/lib/aarch64-linux-gnu/"
rm -rf "$np"

# --- host protoc / grpc_cpp_plugin matching the sysroot's libprotobuf ----
# The libprotobuf-using FCs (services/com, services/per) generate .pb.{cc,h}
# with the HOST protoc but LINK the sysroot's libprotobuf. Those MUST be the
# same protobuf release or the generated headers `#error regenerate ... newer
# protoc`. Debian bookworm ships protobuf 3.21.12 + grpc 1.51.1; if the host's
# /usr/bin/protoc is older (e.g. Ubuntu 22.04 = 3.12), the cross-build of com/
# per fails. Stage host-x86 protoc 3.21.12 + grpc_cpp_plugin 1.51.1 (+ their
# runtime .so's) into the sysroot's bin so the cmake/genrule codegen runs them
# on the build host while producing 3.21-compatible output:
#   - protoc 3.21.12:    github protobuf v21.12 linux-x86_64 release zip
#   - grpc plugin 1.51:  debian bookworm protobuf-compiler-grpc_1.51.1 amd64 +
#                        libgrpc_plugin_support / libprotoc.so.32 (x86 runtimes)
# (TODO: this is the one piece not yet scripted end-to-end — see
#  docs/tasks/BACKLOG/cross-compile-rpi4.md. The codegen-tool/libprotobuf version
#  alignment is the remaining gate for cross-building com+per to aarch64.)
echo "==> NOTE: ensure host protoc/grpc_cpp_plugin match the sysroot protobuf" \
     "(bookworm = 3.21.12 / grpc 1.51) before cross-building services/com+per."

# --- sanity check ---------------------------------------------------

echo
echo "==> sysroot ready at $TARGET ($(sudo du -sh "$TARGET" | cut -f1))"

missing=()
for lib in libyaml-cpp.so.0.7 libprotobuf.so libgrpc++.so libabsl_strings.so libprotobuf-nanopb.a; do
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

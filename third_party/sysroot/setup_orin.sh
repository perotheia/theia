#!/usr/bin/env bash
# third_party/sysroot/setup_orin.sh — bootstrap an aarch64 Ubuntu JAMMY sysroot
# for cross-compiling Theia to the Jetson Orin (Nano) — L4T r36 userspace is
# Ubuntu 22.04 (glibc 2.35).
#
# WHY ITS OWN SYSROOT (third aarch64 ABI): bookworm binaries reference
# GLIBC_2.36 → won't load on jammy's 2.35; focal (2.31) binaries would load but
# jammy is its own distro key. The build host being jammy itself makes gcc-11
# aarch64 cross a perfect match (glibc 2.35 / GLIBCXX ≤ 3.4.30).
#
# WHY A FROM-SOURCE GRPC CLOSURE (the jetson/focal lesson, cross-compiled
# instead of built on-device): jammy apt ships protobuf 3.12 / grpc 1.30 — too
# old for the Theia codegen contract (protoc 3.21 / grpc 1.51). So this script
# CROSS-BUILDS grpc v1.51.1 (+ its protobuf 3.21.6 + abseil + re2 + c-ares +
# zlib submodules) as STATIC PIC archives into <sysroot>/usr/local — the
# binaries then carry no grpc/protobuf runtime deps at all (deb Depends stays
# "theia-runtime", matching the is_arm64 packaging branch). Codegen during the
# closure build uses the staged HOST-x86 toolset (codegen-bookworm-x86: protoc
# 3.21.12 + grpc_cpp_plugin 1.51 — same versions, x86 binaries).
#
# What ends up in the sysroot:
#   - minbase jammy arm64 rootfs (ubuntu-ports)
#   - jammy -dev debs: yaml-cpp, ssl, nanopb, boost {system,thread,random}, zlib
#   - /usr/local: static grpc-1.51.1/protobuf-3.21/absl closure (+ cmake configs)
#
# Prerequisites: debootstrap, qemu-user-static, gcc/g++-aarch64-linux-gnu (11.x),
# cmake ≥ 3.22, git. Idempotent per phase (rootfs / debs / closure).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="$SCRIPT_DIR/orin"
SUITE="jammy"
MIRROR="http://ports.ubuntu.com/ubuntu-ports"
CG_DIR="$SCRIPT_DIR/../codegen-bookworm-x86"     # host-x86 protoc 3.21 + grpc plugin
GRPC_TAG=v1.51.1
JOBS=${JOBS:-$(nproc)}

# ── Phase 1: rootfs ─────────────────────────────────────────────────────────
if [ ! -e "$TARGET/etc/os-release" ]; then
    sudo debootstrap --arch=arm64 --variant=minbase \
        --include=ca-certificates "$SUITE" "$TARGET" "$MIRROR"
    # universe for yaml-cpp/nanopb
    echo "deb $MIRROR $SUITE main universe" | sudo tee "$TARGET/etc/apt/sources.list" >/dev/null
    echo "deb $MIRROR $SUITE-updates main universe" | sudo tee -a "$TARGET/etc/apt/sources.list" >/dev/null
fi

# ── Phase 2: the -dev library set (chroot under qemu) ───────────────────────
if [ ! -e "$TARGET/usr/include/yaml-cpp/yaml.h" ]; then
    sudo cp /usr/bin/qemu-aarch64-static "$TARGET/usr/bin/" 2>/dev/null || true
    sudo chroot "$TARGET" /bin/bash -c "
        set -e
        apt-get update -qq
        DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
            libyaml-cpp-dev libssl-dev zlib1g-dev \
            libnanopb-dev \
            libboost-system-dev libboost-thread-dev libboost-random-dev \
            libnftables-dev libbpf-dev libelf-dev \
            libmnl-dev libnl-3-dev libnl-route-3-dev
        # NOT the (generator) nanopb package: it Depends protobuf-compiler →
        # libprotobuf-dev, whose 3.12 headers land in /usr/include and SHADOW
        # the /usr/local 3.21 closure headers at cross-compile (the .pb.h
        # newer-protoc #error). The generator runs on the HOST; the sysroot
        # needs only libnanopb-dev (pb.h + libprotobuf-nanopb.a).
        apt-get remove -y -qq libprotobuf-dev protobuf-compiler 2>/dev/null || true
        apt-get clean"
fi

# ── Phase 3: cross-built static grpc closure → <sysroot>/usr/local ─────────
if [ ! -e "$TARGET/usr/local/lib/libgrpc++.a" ]; then
    [ -x "$CG_DIR/bin/protoc" ] || { echo "missing $CG_DIR/bin/protoc (staged codegen toolset)"; exit 1; }
    WORK="$SCRIPT_DIR/.orin-grpc-build"
    if [ ! -d "$WORK/grpc" ]; then
        mkdir -p "$WORK" && cd "$WORK"
        git clone --depth 1 -b "$GRPC_TAG" https://github.com/grpc/grpc
        cd grpc && git submodule update --init --depth 1 \
            third_party/abseil-cpp third_party/protobuf third_party/re2 \
            third_party/cares/cares third_party/zlib
    fi
    cd "$WORK/grpc"
    # A tiny toolchain file (self-contained — this script must run before the
    # repo's generic cmake/toolchain-cross.cmake env conventions apply).
    cat > /tmp/orin-toolchain.cmake <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_SYSROOT $TARGET)
set(CMAKE_FIND_ROOT_PATH $TARGET)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
    rm -rf cmake/build-orin && mkdir -p cmake/build-orin && cd cmake/build-orin
    LD_LIBRARY_PATH="$CG_DIR/lib" cmake ../.. \
        -DCMAKE_TOOLCHAIN_FILE=/tmp/orin-toolchain.cmake \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_SHARED_LIBS=OFF \
        -DgRPC_INSTALL=ON \
        -DgRPC_BUILD_TESTS=OFF \
        -DgRPC_BUILD_CODEGEN=OFF \
        -DgRPC_BUILD_GRPC_CPP_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_CSHARP_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_NODE_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_PHP_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_PYTHON_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_RUBY_PLUGIN=OFF \
        -DgRPC_SSL_PROVIDER=package \
        -DgRPC_ZLIB_PROVIDER=module \
        -DgRPC_CARES_PROVIDER=module \
        -DgRPC_RE2_PROVIDER=module \
        -DgRPC_ABSL_PROVIDER=module \
        -DgRPC_PROTOBUF_PROVIDER=module \
        -D_gRPC_PROTOBUF_PROTOC_EXECUTABLE="$CG_DIR/bin/protoc" \
        -DCMAKE_INSTALL_PREFIX=/usr/local
    LD_LIBRARY_PATH="$CG_DIR/lib" make -j"$JOBS"
    sudo make install DESTDIR="$TARGET"
fi

echo "orin sysroot ready: $TARGET"
ls "$TARGET/usr/local/lib/libgrpc++.a" "$TARGET/usr/local/lib/libprotobuf.a" \
   "$TARGET/usr/include/yaml-cpp/yaml.h" 2>/dev/null || true

#!/usr/bin/env bash
# third_party/sysroot/setup_rpi4.sh — bootstrap an aarch64 Debian BOOKWORM
# sysroot for cross-compiling Theia C++ binaries to Raspberry Pi 4.
#
# SUITE = bookworm DELIBERATELY, even though the live rpi4 (rig1-central) runs
# trixie (Debian 13). The build host (Ubuntu 22.04) only has the gcc-11/12
# aarch64 cross-toolchain (glibc ≤2.36 / GLIBCXX_3.4.30); a TRIXIE sysroot's
# libgrpc/libprotobuf reference glibc 2.38 + GLIBCXX_3.4.32 symbols
# (__isoc23_*, std::ios_base_library_init, __cxa_call_terminate) that gcc-11
# cannot resolve → the link FAILS. bookworm (glibc 2.36) matches gcc-11/12, and
# glibc is FORWARD-compatible: a bookworm-linked aarch64 binary runs fine on the
# trixie Pi. (A trixie sysroot would need a gcc-13+ cross-toolchain, which isn't
# packaged for 22.04 — revisit if the build host moves to 24.04+.)
#
# bookworm keeps protobuf 3.21.12 / grpc 1.51.1, so the host-x86 codegen toolset
# below (bookworm 3.21.12 / grpc 1.51) matches the sysroot's libprotobuf.
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
SUITE="bookworm"   # Debian 12 — matches the gcc-11/12 cross-toolchain; runs on the
                   # trixie Pi via glibc forward-compat. See the header note.
MIRROR="http://deb.debian.org/debian"

# The library set the supervisor + services-com link against. Match
# names from `dpkg -l | grep -E 'libgrpc|libyaml|libprotobuf|libabsl'`
# on a clean Debian bookworm install.
#
# services/per static-links etcd-cpp-apiv3 (//third_party:etcd_cpp, a foreign_cc
# cmake build) — but ONLY the `-core` library (SyncClient + Watcher, pure gRPC).
# cpprestsdk is used ONLY by etcd's Client.cpp (the ASYNC wrapper, pplx::task) —
# which is NOT in -core and NOT used by per. find_package(cpprestsdk) is QUIET
# (optional), so we do NOT install libcpprest-dev: it's a dead dep (verified —
# per's binary has 0 cpprest/pplx symbols). Boost (system/thread/random) IS
# needed: -core's SyncClient uses boost::algorithm::split, Watcher boost::asio.
PACKAGES=(
    libyaml-cpp-dev
    libprotobuf-dev
    libgrpc++-dev
    libgrpc-dev
    libabsl-dev
    # libgrpc-dev ships grpc's CMake config (gRPCTargets.cmake), which exports
    # an IMPORTED gRPC::grpc_cpp_plugin pointing at <sysroot>/bin/grpc_cpp_plugin
    # and FATAL-errors if that file is absent — even a consumer like etcd that
    # only links the libs trips the imported-target existence check at
    # find_package(gRPC). protobuf-compiler-grpc provides that (aarch64) binary,
    # satisfying the check (we never EXEC it cross — codegen uses the host
    # toolset). protobuf-compiler likewise provides bin/protoc.
    protobuf-compiler-grpc
    protobuf-compiler
    # etcd-cpp-apiv3 -core (services/per) static-link deps — boost + openssl.
    # NO libcpprest-dev: cpprest is only Client.cpp's async wrapper, which -core
    # excludes (see the header note above).
    libboost-system-dev
    libboost-thread-dev
    libboost-random-dev
    libssl-dev
    # Per-FC native libs (the Linux-mapping FCs link these directly):
    libnftables-dev    # services/fw  — nftables ruleset (libnftables.h)
    libbpf-dev         # services/idsm — eBPF loader (bpf/libbpf.h)
    libelf-dev         #               — libbpf's ELF dep
    libmnl-dev         # services/nm  — minimal netlink
    libnl-3-dev        # services/nm  — libnl link/addr
    libnl-route-3-dev
    libeigen3-dev      # services/osi — V2V SLAM estimator (header-only; the osi
                       #   impl BUILD pulls it sysroot-relative via -I=/usr/include/eigen3)
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
# package configs (gRPC, pulled in by etcd-cpp-apiv3) bake
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

# --- host-x86 codegen toolset matching the sysroot's libprotobuf -----------
# The libprotobuf-using FCs (services/com, services/per) generate .pb.{cc,h}
# with a HOST-x86 protoc but LINK the sysroot's (aarch64) libprotobuf. The
# codegen protoc/grpc_cpp_plugin MUST be the SAME protobuf release as the
# sysroot (bookworm 3.21.12 / grpc 1.51.1) or the generated headers
# `#error regenerate ... newer protoc`. The sysroot's own protoc is aarch64
# (can't exec on x86); the dev host's is usually older (Ubuntu 22.04 = 3.12).
# So we stage a host-x86 protoc 3.21.12 + grpc_cpp_plugin 1.51.1 + their runtime
# .so closure under third_party/codegen-bookworm-x86/{bin,lib} (gitignored host
# data, ~28 MB). The com proto genrules (services/com/BUILD.bazel) and the etcd
# foreign_cc cmake (third_party/BUILD.bazel) prepend this dir to PATH/
# LD_LIBRARY_PATH for `--platforms=//rules/config:rpi4` builds (the _CG_DIR
# selects). We assemble it by fetching the bookworm AMD64 .debs (same upstream
# versions as the arm64 sysroot) and extracting their bin+lib:
#   protobuf-compiler 3.21.12, libprotoc32, libprotobuf32 (protoc + libprotoc.so)
#   protobuf-compiler-grpc 1.51.1, libgrpc++1.51 / libgrpc29 / libgpr29
#     (grpc_cpp_plugin + libgrpc_plugin_support.so)
#   libabsl20220623, libre2-9, libc-ares2 (their transitive runtime .so's)
# DANGER: do NOT `sudo cp` these 3.21 bins over the HOST /usr/bin — that breaks
# the x86 (host-libprotobuf 3.12) build. They live ONLY under third_party/
# codegen-bookworm-x86/, reached via LD_LIBRARY_PATH, never on the system PATH.
CG_DIR="$SCRIPT_DIR/../codegen-bookworm-x86"
if [[ -x "$CG_DIR/bin/protoc" && -x "$CG_DIR/bin/grpc_cpp_plugin" ]]; then
    echo "==> codegen toolset already staged at $CG_DIR (skipping)"
else
    echo "==> staging host-x86 bookworm codegen toolset (protoc 3.21.12 / grpc 1.51)"
    mkdir -p "$CG_DIR/bin" "$CG_DIR/lib"
    cgtmp="$(mktemp -d)"
    BW="http://ftp.debian.org/debian/pool/main"
    # Resolve each .deb DYNAMICALLY by <name>_<upstream-version> prefix, picking
    # the highest Debian revision currently in the pool. Hardcoding a full
    # filename (…_3.21.12-3_amd64.deb) rots: Debian point-releases bump the
    # revision suffix (-3 → -3+b1 → -16) and drop the old file → 404. The SONAME
    # is unchanged across revisions of one upstream version (libprotoc.so.32,
    # libgrpc++.so.1.51), so any revision of 3.21.12 / 1.51.1 is ABI-compatible
    # with the (same-upstream) arm64 sysroot — exactly what the codegen needs.
    #
    # Each entry: "<pool-subdir> <name>_<upstream-prefix>". The upstream prefix
    # is anchored so e.g. libgrpc29 never matches libgrpc++1.51.
    SPECS=(
        "p/protobuf protobuf-compiler_3.21.12"
        "p/protobuf libprotoc32_3.21.12"
        "p/protobuf libprotobuf32_3.21.12"
        "g/grpc protobuf-compiler-grpc_1.51.1"
        "g/grpc libgrpc++1.51_1.51.1"
        "g/grpc libgrpc29_1.51.1"
        # NB: libgpr is no longer a separate package — newer grpc revisions bundle
        # libgpr.so into libgrpc29, so do not fetch libgpr29 (it 404s in the pool).
        "a/abseil libabsl20220623_20220623.1"
        "r/re2 libre2-9_2022"
        "c/c-ares libc-ares2_1.18"
    )
    for spec in "${SPECS[@]}"; do
        subdir="${spec%% *}"; prefix="${spec##* }"
        # List the pool dir, keep amd64 debs whose name starts with the prefix,
        # version-sort, take the newest.
        fname="$(curl -fsSL "$BW/$subdir/" 2>/dev/null \
            | grep -oE "${prefix//+/\\+}[^\"]*_amd64\.deb" \
            | sort -V | tail -1)"
        if [[ -z "$fname" ]]; then
            err "could not resolve a .deb for '$prefix' in pool/main/$subdir"
            exit 3
        fi
        f="$cgtmp/$fname"
        curl -fsSL "$BW/$subdir/$fname" -o "$f" || { err "fetch failed: $BW/$subdir/$fname"; exit 3; }
        dpkg-deb -x "$f" "$cgtmp/root"
    done
    cp "$cgtmp/root/usr/bin/protoc"          "$CG_DIR/bin/"
    cp "$cgtmp/root/usr/bin/grpc_cpp_plugin" "$CG_DIR/bin/"
    # The well-known protos (google/protobuf/*.proto) must be on protoc's import
    # path so etcd's `.proto`s (which import google/protobuf/* + gogoproto) resolve.
    # The protobuf-compiler deb does NOT ship them under /usr/include (that's
    # libprotobuf-dev) — but the arm64 SYSROOT already has them (same upstream
    # 3.21.12), so copy from there. gogoproto comes vendored in the etcd tree.
    mkdir -p "$CG_DIR/include"
    if [[ -d "$TARGET/usr/include/google/protobuf" ]]; then
        cp -r "$TARGET/usr/include/google" "$CG_DIR/include/"
    elif [[ -d "$cgtmp/root/usr/include/google" ]]; then
        cp -r "$cgtmp/root/usr/include/google" "$CG_DIR/include/"
    else
        err "WARN: google/protobuf well-known protos not found (sysroot or deb) — "\
"etcd cross-codegen may fail on google/protobuf imports"
    fi
    # the .so closure both bins dlopen/link (libprotoc, libgrpc_plugin_support,
    # the abseil/grpc/protobuf/re2/c-ares runtimes).
    find "$cgtmp/root/usr/lib" -name '*.so*' -exec cp -P {} "$CG_DIR/lib/" \;
    rm -rf "$cgtmp"
    if LD_LIBRARY_PATH="$CG_DIR/lib" "$CG_DIR/bin/protoc" --version | grep -q '3.21.12'; then
        echo "==> codegen toolset ready: $(LD_LIBRARY_PATH="$CG_DIR/lib" "$CG_DIR/bin/protoc" --version)"
    else
        err "WARN: staged protoc does not report 3.21.12 — check the .deb versions"
    fi
fi

# --- gRPC plugin at the SYSROOT path (for services/per's etcd cmake) --------
# libgrpc-dev's CMake config (gRPCTargets.cmake) exports IMPORTED gRPC::grpc_*
# _plugin targets at <sysroot>/bin/grpc_*_plugin, and FATAL-errors at
# find_package(gRPC) if those files are absent — even etcd, which only LINKS the
# libs, trips the check. Worse, etcd's CMakeLists reads the cpp plugin via
# `get_target_property(GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin LOCATION)` and runs
# it for codegen — so the file at that path must be a RUNNABLE binary on the
# build host, i.e. the HOST-x86 plugin (the arm64 one can't exec, "Exec format
# error"). So: drop the host-x86 grpc_cpp_plugin (from CG_DIR) at the sysroot
# path, and stub the php/python/ruby plugins (existence-check only, never run).
# The cpp plugin needs its host .so's — third_party/BUILD.bazel's etcd cmake env
# already puts CG_DIR/lib on LD_LIBRARY_PATH.
if [[ -x "$CG_DIR/bin/grpc_cpp_plugin" ]]; then
    echo "==> staging host-x86 grpc_cpp_plugin at the sysroot path (etcd find_package)"
    sudo mkdir -p "$TARGET/usr/bin"
    sudo cp "$CG_DIR/bin/grpc_cpp_plugin" "$TARGET/usr/bin/grpc_cpp_plugin"
    for p in grpc_php_plugin grpc_python_plugin grpc_ruby_plugin; do
        sudo ln -sfn grpc_cpp_plugin "$TARGET/usr/bin/$p"
    done
fi

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

#!/usr/bin/env bash
# third_party/sysroot/setup_codegen_x86.sh — stage the HOST-x86 proto codegen
# toolset (protoc 3.21.12 + grpc_cpp_plugin 1.51.1 + their .so closure) under
# third_party/codegen-bookworm-x86/{bin,lib,include} (gitignored host data).
#
# The same recipe as the "host-x86 codegen toolset" section of setup_rpi4.sh,
# as a standalone step so a sysroot script for another target with the SAME
# protobuf/grpc release (trixie: 3.21.12 / 1.51.1 — setup_ai64.sh) can share
# it. See setup_rpi4.sh for why the versions must match the sysroot.
#
#   setup_codegen_x86.sh [<sysroot>]   # <sysroot> = where to copy the
#                                      # google/protobuf well-known protos from
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CG_DIR="$SCRIPT_DIR/../codegen-bookworm-x86"
SYSROOT="${1:-}"
err() { echo "setup_codegen_x86: $*" >&2; }

if [[ -x "$CG_DIR/bin/protoc" && -x "$CG_DIR/bin/grpc_cpp_plugin" ]]; then
    echo "==> codegen toolset already staged at $CG_DIR (skipping)"
    exit 0
fi

echo "==> staging host-x86 bookworm codegen toolset (protoc 3.21.12 / grpc 1.51)"
mkdir -p "$CG_DIR/bin" "$CG_DIR/lib" "$CG_DIR/include"
cgtmp="$(mktemp -d)"
trap 'rm -rf "$cgtmp"' EXIT
BW="http://ftp.debian.org/debian/pool/main"
# "<pool-subdir> <name>_<upstream-prefix>": the newest Debian revision of that
# upstream version in the pool (a hardcoded full filename rots).
SPECS=(
    "p/protobuf protobuf-compiler_3.21.12"
    "p/protobuf libprotoc32_3.21.12"
    "p/protobuf libprotobuf32_3.21.12"
    "g/grpc protobuf-compiler-grpc_1.51.1"
    "g/grpc libgrpc++1.51_1.51.1"
    "g/grpc libgrpc29_1.51.1"
    "a/abseil libabsl20220623_20220623.1"
    "r/re2 libre2-9_2022"
    "c/c-ares libc-ares2_1.18"
)
for spec in "${SPECS[@]}"; do
    subdir="${spec%% *}"; prefix="${spec##* }"
    fname="$(curl -fsSL "$BW/$subdir/" 2>/dev/null \
        | grep -oE "${prefix//+/\\+}[^\"]*_amd64\.deb" \
        | sort -V | tail -1)"
    if [[ -z "$fname" ]]; then
        err "could not resolve a .deb for '$prefix' in pool/main/$subdir"
        exit 3
    fi
    curl -fsSL "$BW/$subdir/$fname" -o "$cgtmp/$fname" \
        || { err "fetch failed: $BW/$subdir/$fname"; exit 3; }
    dpkg-deb -x "$cgtmp/$fname" "$cgtmp/root"
done
cp "$cgtmp/root/usr/bin/protoc"          "$CG_DIR/bin/"
cp "$cgtmp/root/usr/bin/grpc_cpp_plugin" "$CG_DIR/bin/"
find "$cgtmp/root/usr/lib" -name '*.so*' -exec cp -P {} "$CG_DIR/lib/" \;
# The well-known protos (google/protobuf/*.proto) for etcd's imports: from the
# sysroot's libprotobuf-dev (same upstream release).
if [[ -n "$SYSROOT" && -d "$SYSROOT/usr/include/google/protobuf" ]]; then
    cp -r "$SYSROOT/usr/include/google" "$CG_DIR/include/"
else
    err "WARN: no <sysroot>/usr/include/google/protobuf — etcd cross-codegen" \
        "may fail on google/protobuf imports"
fi
if LD_LIBRARY_PATH="$CG_DIR/lib" "$CG_DIR/bin/protoc" --version | grep -q '3.21.12'; then
    echo "==> codegen toolset ready: $(LD_LIBRARY_PATH="$CG_DIR/lib" "$CG_DIR/bin/protoc" --version)"
else
    err "staged protoc does not report 3.21.12 — check the .deb versions"
    exit 3
fi

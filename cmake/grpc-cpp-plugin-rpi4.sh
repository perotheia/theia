#!/usr/bin/env bash
# cmake/grpc-cpp-plugin-rpi4.sh — wrapper for the sysroot's
# grpc_cpp_plugin so it runs under qemu-user-static. Mirrors
# protoc-rpi4.sh.
#
# protoc invokes this as a subprocess (--plugin=protoc-gen-grpc=...).
# The wrapper must speak the same stdin/stdout protoc-plugin protocol
# the binary expects — qemu just transparently relays.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SYSROOT="${RPI4_SYSROOT:-$(cd "$SCRIPT_DIR/.." && pwd)/third_party/sysroot/rpi4}"

exec qemu-aarch64-static -L "$SYSROOT" "$SYSROOT/usr/bin/grpc_cpp_plugin" "$@"

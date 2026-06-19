#!/usr/bin/env bash
# Cross-compile the supervisor to aarch64 (Raspberry Pi 4, Debian bookworm) and
# pack it into a theia-runtime arm64 .deb — RUNTIME ONLY (the supervisor binary).
#
# Prereqs: gcc-aarch64-linux-gnu (apt), the bookworm aarch64 sysroot at
# third_party/sysroot/rpi4 (third_party/sysroot/setup_rpi4.sh), and nanopb staged
# into the sysroot (setup_rpi4.sh does this). Bazel selects the aarch64 toolchain
# via `--config=rpi4` (//rules/config:rpi4 + //toolchains/aarch64_linux_gnu).
set -euo pipefail

ROOT="$(pwd)"
VERSION="${THEIA_VERSION:-0.1.0}"
export THEIA_RPI4_SYSROOT="${THEIA_RPI4_SYSROOT:-$ROOT/third_party/sysroot/rpi4}"

# 1. Cross-build the supervisor (bazel, aarch64 toolchain).
bazel build //platform/supervisor/main:supervisor --config=rpi4
SUP="$ROOT/bazel-bin/platform/supervisor/main/supervisor"
file "$SUP" | grep -q "ARM aarch64" || { echo "supervisor is not aarch64"; exit 1; }

# 2. Stage the arm64 runtime .deb (just the supervisor — the fabric).
STAGE="$(mktemp -d)"
opt="$STAGE/opt/theia"
mkdir -p "$opt/bin"
install -m 0755 "$SUP" "$opt/bin/supervisor"

ctrl="$STAGE/DEBIAN"
mkdir -p "$ctrl"
installed_kb="$(du -sk "$opt" | cut -f1)"
# Depends recomputed by fix-deb-depends.sh against the sysroot via aarch64
# dpkg-shlibdeps would need an arm64 dpkg-shlibdeps; instead declare the bookworm
# runtime libs the supervisor links (glibc + libstdc++; nanopb is static).
cat > "$ctrl/control" <<EOF
Package: theia-runtime
Version: $VERSION
Architecture: arm64
Section: misc
Priority: optional
Maintainer: Theia <theia@example.com>
Installed-Size: $installed_kb
Depends: libc6 (>= 2.34), libstdc++6 (>= 12)
Description: Theia runtime: the supervisor binary (run-time fabric) — Raspberry Pi 4 (aarch64).
 Cross-compiled for aarch64 / Debian bookworm (Raspberry Pi OS 12). The fabric
 an app system runs under.
EOF

out="$ROOT/dist/debian/theia-runtime"
mkdir -p "$out"
deb="$out/theia-runtime_${VERSION}_arm64.deb"
dpkg-deb --build --root-owner-group "$STAGE" "$deb"
rm -rf "$STAGE"
echo "theia-runtime (arm64) .deb → $deb"
dpkg-deb -I "$deb" | grep -E "Package|Architecture|Depends"

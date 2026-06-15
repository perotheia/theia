#!/usr/bin/env bash
# Build the supervisor-GUI (wxWidgets + gRPC CMake app) and package it — with
# the rtdb python client — into a theia-tools .deb. The GUI is built by its
# canonical CMakeLists.txt (the source of truth; not bazel). Runtime Depends are
# left to the shared dpkg-shlibdeps pass (.github/scripts/fix-deb-depends.sh),
# so wx/gtk/grpc sonames resolve per Ubuntu release.
#
# Usage: build-tools-deb.sh <ubuntu-version>   (e.g. 22.04)
# Prereqs on PATH: cmake, protoc, grpc_cpp_plugin, libwxgtk3.2-dev, libgrpc++-dev,
#   libprotobuf-dev, and third_party/etcd-cpp-apiv3/install/ already built.
set -euo pipefail

UBUNTU="${1:-unknown}"
ROOT="$(pwd)"
VERSION="${THEIA_VERSION:-0.1.0}"
GUI_SRC="$ROOT/tools/supervisor-gui"
BUILD="$ROOT/build-gui"

# 1. Build the GUI binary via CMake (the canonical build).
cmake -S "$GUI_SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j"$(nproc)"
test -x "$BUILD/supervisor-gui" || { echo "GUI build produced no binary"; exit 1; }

# 2. Stage the .deb tree.
STAGE="$(mktemp -d)"
opt="$STAGE/opt/theia"
mkdir -p "$opt/bin" "$opt/lib/python3/rtdb"

install -m 0755 "$BUILD/supervisor-gui" "$opt/bin/supervisor-gui"

# rtdb — the python gRPC debug client (deps satisfied by the framework wheels in
# the user's venv). Ship the package + a PATH shim that runs it from the venv.
cp -r "$ROOT/tools/rtdb/." "$opt/lib/python3/rtdb/"
cat > "$opt/bin/rtdb" <<'SHIM'
#!/bin/sh
# rtdb — Remote Theia Debug bridge (tdb over com's gRPC, :7700). Runs from the
# active venv's python (grpc comes from the framework wheels); the rtdb package
# ships under /opt/theia/lib/python3.
D="$(cd "$(dirname "$0")/.." && pwd)"
exec python3 -c "import sys; sys.path.insert(0, '$D/lib/python3'); from rtdb.rtdb import main; main()" "$@"
SHIM
chmod 0755 "$opt/bin/rtdb"

# 3. control — Depends are a fallback; dpkg-shlibdeps rewrites the GUI's later.
ctrl="$STAGE/DEBIAN"
mkdir -p "$ctrl"
installed_kb="$(du -sk "$opt" | cut -f1)"
cat > "$ctrl/control" <<EOF
Package: theia-tools
Version: $VERSION
Architecture: amd64
Section: utils
Priority: optional
Maintainer: Theia <theia@robofortis.com>
Installed-Size: $installed_kb
Depends: theia-services, python3
Description: Theia operator tools — the supervisor GUI (wxWidgets) + rtdb.
 The supervisor-gui and rtdb speak the com Functional Cluster's gRPC surface
 (SupervisorView / TraceStream on :7700) to observe and drive a running stack.
 The GUI links wxWidgets 3.2 (native to Ubuntu 24.04+).
EOF

# 4. Build into the standard dist layout so the release steps pick it up.
out="$ROOT/dist/debian/theia-tools"
mkdir -p "$out"
deb="$out/theia-tools_${VERSION}_amd64.deb"
dpkg-deb --build --root-owner-group "$STAGE" "$deb"
rm -rf "$STAGE"
echo "theia-tools .deb → $deb"

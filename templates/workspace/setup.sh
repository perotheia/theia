#!/usr/bin/env bash
# One-time workspace setup: point the platform/ wrappers at the runtime sources
# installed by the theia-runtime deb (/opt/theia/src). Re-run after upgrading the
# deb. Override the install root with THEIA_ROOT (default /opt/theia).
set -euo pipefail

THEIA_ROOT="${THEIA_ROOT:-/opt/theia}"
SRC="$THEIA_ROOT/src"

if [ ! -d "$SRC/runtime/include" ]; then
  echo "setup: $SRC/runtime not found — install the theia-runtime deb first:" >&2
  echo "  sudo apt install ./theia-runtime_*.deb" >&2
  exit 1
fi

here="$(cd "$(dirname "$0")" && pwd)"
ln -sfn "$SRC/runtime"              "$here/platform/runtime/runtime_src"
ln -sfn "$SRC/supervisor/tombstone" "$here/platform/supervisor/tombstone/tombstone_src"

# The runtime's .art package (defines ChildControlIf, the LogLevel/TraceControl
# push interfaces). Services import `platform.runtime.*`; that resolves to
# platform/runtime/package.art — which must point at the installed runtime spec
# so `artheia executor emit`/`generate-manifest` can parse a service .art
# (e.g. per) downstream. Mirrors the in-repo
# platform/runtime/package.art → system/runtime/package.art symlink.
ln -sfn "runtime_src/system/runtime/package.art" "$here/platform/runtime/package.art"
# Also expose it as system/runtime (the canonical import path) for any .art that
# imports the runtime via that route.
mkdir -p "$here/system"
ln -sfn "$SRC/runtime/system/runtime" "$here/system/runtime"

echo "setup: wired platform/runtime → $SRC/runtime"
echo "setup: wired platform/runtime/package.art → the runtime .art (ChildControlIf)"
echo "setup: wired platform/supervisor/tombstone → $SRC/supervisor/tombstone"
echo "Now: artheia gen-app --kind fc system/myapp/package.art --out myapp --ns my::app"
echo "Then: bazel build //myapp/..."

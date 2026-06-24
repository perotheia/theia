#!/usr/bin/env bash
# theia-run — start the Theia supervisor for a FLAT /opt/theia deploy (the .deb
# layout: /opt/theia/{bin,config,lib}). The supervisor binary is STRICT — it
# requires THEIA_ROOT_DIR + THEIA_SUPERVISOR_MANIFEST in the env and refuses to
# start otherwise (no cwd guessing). This launcher is the ONE place that exports
# them, so the operator / systemd / `theia start` just runs this script.
#
# Works in BOTH envs (x86 dev box + aarch64 rpi4): the install root is
# THEIA_ROOT_DIR (default /opt/theia), not guessed. ONE env name — the same
# THEIA_ROOT_DIR the supervisor binary reads — so there's no second "root" knob.
set -euo pipefail

# Install root. The supervisor binary REQUIRES this (children's ./bin/<svc>
# resolve against it). Default to the .deb prefix; override for a dev stage dir.
export THEIA_ROOT_DIR="${THEIA_ROOT_DIR:-/opt/theia}"

[[ -x "$THEIA_ROOT_DIR/bin/supervisor" ]] || {
    echo "theia-run: no supervisor at $THEIA_ROOT_DIR/bin (set THEIA_ROOT_DIR)" >&2
    exit 1
}
[[ -f "$THEIA_ROOT_DIR/config/executor.json" ]] || {
    echo "theia-run: no manifest at $THEIA_ROOT_DIR/config/executor.json" >&2
    exit 1
}

# The rest of the strict env, derived from the one root.
export THEIA_SUPERVISOR_MANIFEST="$THEIA_ROOT_DIR/config/executor.json"
export THEIA_LOG_LEVEL="${THEIA_LOG_LEVEL:-info}"
export LD_LIBRARY_PATH="$THEIA_ROOT_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# cd to the root too so an un-prefixed start_cmd also resolves.
cd "$THEIA_ROOT_DIR"
exec "$THEIA_ROOT_DIR/bin/supervisor" "$@"

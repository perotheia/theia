#!/usr/bin/env bash
# theia-run — start the Theia supervisor for a FLAT /opt/theia deploy (the .deb
# layout: /opt/theia/{bin,config,lib}). The supervisor binary is STRICT — it
# requires THEIA_ROOT_DIR + THEIA_SUPERVISOR_MANIFEST in the env and refuses to
# start otherwise (no cwd guessing). This launcher is the ONE place that exports
# them, so the operator / systemd / `theia start` just runs this script.
#
# Works in BOTH envs (x86 dev box + aarch64 rpi4): THEIA_ROOT is taken from the
# install prefix (default /opt/theia, override with THEIA_ROOT=...), not guessed.
set -euo pipefail

# Install prefix. Override for a non-default location (e.g. a dev stage dir).
THEIA_ROOT="${THEIA_ROOT:-/opt/theia}"

[[ -x "$THEIA_ROOT/bin/supervisor" ]] || {
    echo "theia-run: no supervisor at $THEIA_ROOT/bin (set THEIA_ROOT)" >&2
    exit 1
}
[[ -f "$THEIA_ROOT/config/executor.json" ]] || {
    echo "theia-run: no manifest at $THEIA_ROOT/config/executor.json" >&2
    exit 1
}

# The strict env the supervisor requires.
export THEIA_ROOT_DIR="$THEIA_ROOT"                       # children's ./bin/<svc> root
export THEIA_SUPERVISOR_MANIFEST="$THEIA_ROOT/config/executor.json"
export THEIA_LOG_LEVEL="${THEIA_LOG_LEVEL:-info}"
# Bundled shared libs (if any) at $THEIA_ROOT/lib.
export LD_LIBRARY_PATH="$THEIA_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# The supervisor resolves each child's relative start_cmd (./bin/<svc>) against
# THEIA_ROOT_DIR; cd there too so an un-prefixed start_cmd is also correct.
cd "$THEIA_ROOT"
exec "$THEIA_ROOT/bin/supervisor" "$@"

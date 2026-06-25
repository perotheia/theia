#!/usr/bin/env bash
# theia-run — start the Theia supervisor for the RELEASE-DIR /opt/theia layout:
#   /opt/theia/bin/supervisor      the UPDATER — fixed, never OTA-swapped
#   /opt/theia/current → releases/<ver>   the SERVICES + APPS the supervisor forks
#   /opt/theia/config/executor.json       the supervised tree
# The supervisor binary is STRICT — it requires THEIA_ROOT_DIR + THEIA_SUPERVISOR_
# MANIFEST and resolves each child's ./bin/<svc> from $THEIA_ROOT_DIR/current (it
# REFUSES to start if `current` is absent — no flat-bin fallback). OTA (Mender
# theia-release / UCM) flips current→releases/<ver> + restarts the FCs; the
# supervisor binary is untouched. This launcher is the ONE place that exports the
# env, so the operator / systemd / `theia start` just runs this script.
set -euo pipefail

# Install root (where the supervisor binary + config live). Default to the .deb
# prefix; override for a dev stage dir.
export THEIA_ROOT_DIR="${THEIA_ROOT_DIR:-/opt/theia}"

[[ -x "$THEIA_ROOT_DIR/bin/supervisor" ]] || {
    echo "theia-run: no supervisor at $THEIA_ROOT_DIR/bin (set THEIA_ROOT_DIR)" >&2
    exit 1
}
[[ -d "$THEIA_ROOT_DIR/current" ]] || {
    echo "theia-run: no release at $THEIA_ROOT_DIR/current — provision must lay the" \
         "releases/<ver> + current symlink (the children run from it)" >&2
    exit 1
}
[[ -f "$THEIA_ROOT_DIR/config/executor.json" ]] || {
    echo "theia-run: no manifest at $THEIA_ROOT_DIR/config/executor.json" >&2
    exit 1
}

# The rest of the strict env, derived from the one root. The CHILDREN's libs come
# from the current release (current/lib); OTA swaps them with current. The
# supervisor passes LD_LIBRARY_PATH down to each FC it forks.
export THEIA_SUPERVISOR_MANIFEST="$THEIA_ROOT_DIR/config/executor.json"
export THEIA_LOG_LEVEL="${THEIA_LOG_LEVEL:-info}"
export LD_LIBRARY_PATH="$THEIA_ROOT_DIR/current/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# cd to the root so the supervisor resolves current/bin/<svc> relative to it.
cd "$THEIA_ROOT_DIR"
exec "$THEIA_ROOT_DIR/bin/supervisor" "$@"

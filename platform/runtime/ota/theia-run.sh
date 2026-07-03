#!/usr/bin/env bash
# theia-run — start the Theia supervisor for the RELEASE-DIR /opt/theia layout:
#   /opt/theia/bin/supervisor      the UPDATER — fixed, never OTA-swapped
#   /opt/theia/current → releases/<ver>   the SERVICES + APPS the supervisor forks
#   /opt/theia/config/executor.json       the supervised tree
#
# THEIA_INSTALL_DIR: colon-separated list of dirs the supervisor searches to
# resolve each child's relative start_cmd (e.g. "bin/crypto"). OTA flips
# current→releases/<ver>; the supervisor binary is untouched. This launcher is
# the ONE place that exports the env.
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
         "releases/<ver> + current symlink" >&2
    exit 1
}
[[ -f "$THEIA_ROOT_DIR/config/executor.json" ]] || {
    echo "theia-run: no manifest at $THEIA_ROOT_DIR/config/executor.json" >&2
    exit 1
}

# THEIA_INSTALL_DIR: the release-dir the supervisor scans to resolve child bins.
# For OTA this is always $THEIA_ROOT_DIR/current (UCM flips this symlink).
export THEIA_INSTALL_DIR="${THEIA_INSTALL_DIR:-$THEIA_ROOT_DIR/current}"
export THEIA_SUPERVISOR_MANIFEST="$THEIA_ROOT_DIR/config/executor.json"
# THEIA_MACHINE_MANIFEST: the dir holding machines.json, which com reads for the
# AUTHORITATIVE instance→machine_index→name map (the unique runtime identity com
# and the GUI key on). Point it at config/ when machines.json is staged there
# (provision copies it alongside executor.json). Without it com falls back to the
# per-supervisor SystemInfo.machine_name — correct once the rig uses unique names,
# but the manifest map resolves a machine even before its supervisor answers.
if [[ -f "$THEIA_ROOT_DIR/config/machines.json" ]]; then
    export THEIA_MACHINE_MANIFEST="${THEIA_MACHINE_MANIFEST:-$THEIA_ROOT_DIR/config}"
fi
export THEIA_LOG_LEVEL="${THEIA_LOG_LEVEL:-info}"
# The CHILDREN's libs come from the current release; OTA swaps them with current.
export LD_LIBRARY_PATH="$THEIA_ROOT_DIR/current/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

exec "$THEIA_ROOT_DIR/bin/supervisor" "$@"

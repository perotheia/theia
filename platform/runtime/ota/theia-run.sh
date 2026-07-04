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

# THEIA_MACHINE / THEIA_MACHINE_INSTANCE: this board's identity. DERIVE them from
# on-device state so EVERY launcher gets it right — colony's supervisor unit, the
# container entrypoint, AND the theia-swp OTA relaunch — without each having to
# pass them. The name is executor.json's root "machine"; the instance is that
# name's machines.json machine_index. This is the single source: an OTA relaunch
# that forgot to pass THEIA_MACHINE_INSTANCE used to boot a worker at instance 0,
# colliding with master in the shared TIPC namespace (com couldn't reach it).
# An explicit env wins (caller override); we only fill what's unset.
if [[ -z "${THEIA_MACHINE:-}" && -f "$THEIA_ROOT_DIR/config/executor.json" ]]; then
    THEIA_MACHINE="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1])).get("machine",""))' \
        "$THEIA_ROOT_DIR/config/executor.json" 2>/dev/null)"
    [[ -n "$THEIA_MACHINE" ]] && export THEIA_MACHINE
fi
if [[ -z "${THEIA_MACHINE_INSTANCE:-}" && -n "${THEIA_MACHINE:-}" \
      && -f "$THEIA_ROOT_DIR/config/machines.json" ]]; then
    _mi="$(python3 -c 'import json,sys
d=json.load(open(sys.argv[1])); idx=d.get("machine_index",{})
v=idx.get(sys.argv[2])
print(v if isinstance(v,int) else "")' \
        "$THEIA_ROOT_DIR/config/machines.json" "$THEIA_MACHINE" 2>/dev/null)"
    # FAIL FAST: a manifest that names this machine MUST carry its index. A silent
    # default to 0 is what caused the collision — surface it instead.
    if [[ -n "$_mi" ]]; then
        export THEIA_MACHINE_INSTANCE="$_mi"
    else
        echo "theia-run: machine '$THEIA_MACHINE' has no machine_index in" \
             "$THEIA_ROOT_DIR/config/machines.json — cannot resolve its TIPC" \
             "instance (refusing to default to 0 and collide)." >&2
        exit 1
    fi
fi

export THEIA_LOG_LEVEL="${THEIA_LOG_LEVEL:-info}"
# The CHILDREN's libs come from the current release; OTA swaps them with current.
export LD_LIBRARY_PATH="$THEIA_ROOT_DIR/current/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

exec "$THEIA_ROOT_DIR/bin/supervisor" "$@"

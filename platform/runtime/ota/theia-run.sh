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
# The supervised tree is read THROUGH `current` (two-plane OTA): with no SWP,
# current → . so current/config = the runtime baseline; after an SWP rollout,
# current → releases/<swp> so current/config = the SWP's self-contained executor.
# One path, both states — the launcher never branches on "is an SWP active".
# Back-compat: if current/config is absent (a legacy relocate-into-release layout
# where config stayed flat), fall back to the flat $ROOT/config.
_cfg_dir="$THEIA_ROOT_DIR/current/config"
[[ -f "$_cfg_dir/executor.json" ]] || _cfg_dir="$THEIA_ROOT_DIR/config"
[[ -f "$_cfg_dir/executor.json" ]] || {
    echo "theia-run: no manifest at $THEIA_ROOT_DIR/current/config/executor.json" \
         "(nor the flat $THEIA_ROOT_DIR/config/executor.json)" >&2
    exit 1
}

# THEIA_INSTALL_DIR: the release-dir the supervisor scans to resolve child bins.
# For OTA this is always $THEIA_ROOT_DIR/current (UCM flips this symlink; with
# current → . the runtime's own $ROOT/bin resolves).
export THEIA_INSTALL_DIR="${THEIA_INSTALL_DIR:-$THEIA_ROOT_DIR/current}"
export THEIA_SUPERVISOR_MANIFEST="$_cfg_dir/executor.json"
# THEIA_MACHINE_MANIFEST: the dir holding machines.json, which com reads for the
# AUTHORITATIVE instance→machine_index→name map (the unique runtime identity com
# and the GUI key on). Point it at config/ when machines.json is staged there
# (provision copies it alongside executor.json). Without it com falls back to the
# per-supervisor SystemInfo.machine_name — correct once the rig uses unique names,
# but the manifest map resolves a machine even before its supervisor answers.
# Read machines.json from the SAME dir as executor.json ($_cfg_dir, through
# current) so an SWP that ships its own machines.json is honored; falls back to
# the flat config dir.
if [[ -f "$_cfg_dir/machines.json" ]]; then
    export THEIA_MACHINE_MANIFEST="${THEIA_MACHINE_MANIFEST:-$_cfg_dir}"
fi

# THEIA_MACHINE_INSTANCE persistence: the board's TIPC instance is a DEPLOY fact
# (colony passes machine_instance per board: master=0, and each zonal worker a
# distinct 1,2,… — it is NOT derivable from the machine NAME, which is shared
# across all zonal workers). The FIRST launcher to receive it (colony's
# supervisor unit) persists it here so any LATER launch on this board — the
# theia-swp OTA relaunch, a manual restart — recovers the SAME instance instead
# of defaulting to 0 (which collided with master in the shared TIPC namespace).
_INST_FILE="$THEIA_ROOT_DIR/config/machine_instance"
if [[ -n "${THEIA_MACHINE_INSTANCE:-}" ]]; then
    # A caller passed it (colony/first boot) — persist for later launches.
    printf '%s\n' "$THEIA_MACHINE_INSTANCE" > "$_INST_FILE" 2>/dev/null || true
elif [[ -r "$_INST_FILE" ]]; then
    # No caller value — recover the persisted deploy-time instance.
    export THEIA_MACHINE_INSTANCE="$(cat "$_INST_FILE")"
fi

export THEIA_LOG_LEVEL="${THEIA_LOG_LEVEL:-info}"
# The CHILDREN's libs come from the current release; OTA swaps them with current.
export LD_LIBRARY_PATH="$THEIA_ROOT_DIR/current/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# TIPC netid (cluster id) — per-rig ISOLATION on a shared L2 / shared host ns.
# A DEPLOY fact like machine_instance: colony passes THEIA_TIPC_NETID per rig
# (e.g. an e2e/test rig picks a non-default id so a dev's `theia start` on the
# same wire can't cross-talk); persisted so an OTA relaunch / manual restart
# recovers it. The netid is per NETWORK NAMESPACE and only changes while no
# TIPC sockets/bearers are up — best-effort with a LOUD diagnostic: a mismatch
# means this rig may see (and be seen by) another cluster's services.
_NETID_FILE="$THEIA_ROOT_DIR/config/tipc_netid"
if [[ -n "${THEIA_TIPC_NETID:-}" ]]; then
    printf '%s\n' "$THEIA_TIPC_NETID" > "$_NETID_FILE" 2>/dev/null || true
elif [[ -r "$_NETID_FILE" ]]; then
    THEIA_TIPC_NETID="$(cat "$_NETID_FILE")"
fi
if [[ -n "${THEIA_TIPC_NETID:-}" ]] && command -v tipc >/dev/null 2>&1; then
    _cur="$(tipc node get netid 2>/dev/null | head -1 | tr -dc '0-9')"
    if [[ "$_cur" != "$THEIA_TIPC_NETID" ]]; then
        if tipc node set netid "$THEIA_TIPC_NETID" 2>/dev/null; then
            echo "theia-run: TIPC netid set to $THEIA_TIPC_NETID (was ${_cur:-unset})"
        else
            echo "theia-run: WARNING — could not set TIPC netid $THEIA_TIPC_NETID" \
                 "(current ${_cur:-unknown}; sockets/bearers already up?). This rig" \
                 "may cross-talk with another cluster on this namespace." >&2
        fi
    fi
fi

exec "$THEIA_ROOT_DIR/bin/supervisor" "$@"

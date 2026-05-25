#!/usr/bin/env bash
# deploy/stage.sh — copy Bazel artifacts to deploy/.staging/.
#
# docker-compose.yml bind-mounts ./.staging/<machine>/ipk into
# /opt/theia/ipk in each container. This script populates .staging/
# from `bazel-bin/external/+rig_ext+rig_demo/`.
#
# Idempotent — re-running overwrites the staging area cleanly.
#
# Usage:  ./deploy/stage.sh [rig_repo_name]
#         (default: rig_demo)

set -euo pipefail

readonly RIG="${1:-rig_demo}"
readonly DEPLOY_DIR="$(cd "$(dirname "$0")" && pwd)"
readonly WORKSPACE="$(cd "$DEPLOY_DIR/.." && pwd)"
readonly BAZEL_RIG_DIR="$WORKSPACE/bazel-bin/external/+rig_ext+${RIG}"

# Prepend the workspace venv to PATH so `artheia` resolves without
# requiring the caller to `source .venv/bin/activate` first.
if [[ -x "$WORKSPACE/.venv/bin/artheia" ]]; then
    export PATH="$WORKSPACE/.venv/bin:$PATH"
fi

log() { echo "[stage] $*" >&2; }

if [[ ! -d "$BAZEL_RIG_DIR" ]]; then
    log "ERROR: $BAZEL_RIG_DIR does not exist."
    log "       Run 'bazel build @${RIG}//:all' first."
    exit 2
fi

# Find the per-machine subdirs (each has its own .ipk).
# Bazel only writes to bazel-bin/external/+rig_ext+<rig>/<machine>/
# when that machine has at least one buildable component (the
# pkg_opkg target actually runs). Machines that are all
# bazel_buildable=False produce an empty filegroup with no output
# directory — they need to be discovered from the rig.json instead.

machines=()

# Primary source of truth: ask the rig manifest directly via
# `artheia rig-deps`. This catches machines with no buildable
# components (which won't show in bazel-bin).
#
# We need the PYTHON module name (e.g. `demo.manifest.rig`), not the
# Bazel-side @rig_<name> alias passed as the first arg to this script.
# For demo this is hardcoded; future rigs add their own mapping by
# extending the case below.

case "$RIG" in
    rig_demo)  RIG_MODULE="demo.manifest.rig" ;;
    *)         RIG_MODULE="" ;;  # unknown rig; skip rig-deps discovery
esac

if [[ -n "$RIG_MODULE" ]] && command -v artheia >/dev/null 2>&1; then
    while IFS= read -r m; do
        machines+=("$m")
    done < <(artheia rig-deps "$RIG_MODULE" 2>/dev/null \
             | python3 -c "import json, sys; d = json.load(sys.stdin); [print(m['name']) for m in d['machines']]" \
             2>/dev/null)
fi

# Fallback: list directories in bazel-bin (only finds machines that
# actually produced outputs).
if (( ${#machines[@]} == 0 )); then
    for d in "$BAZEL_RIG_DIR"/*/ ; do
        [[ -d "$d" ]] || continue
        machines+=("$(basename "$d")")
    done
fi

if (( ${#machines[@]} == 0 )); then
    log "ERROR: no machines discovered (rig-deps + bazel-bin both empty)"
    exit 3
fi

log "rig: $RIG  machines: ${machines[*]}"

# Clean + repopulate the staging area.
rm -rf "$DEPLOY_DIR/.staging"
mkdir -p "$DEPLOY_DIR/.staging"

for m in "${machines[@]}"; do
    src_dir="$BAZEL_RIG_DIR/$m"
    dst_dir="$DEPLOY_DIR/.staging/$m/ipk"
    mkdir -p "$dst_dir"

    # The .ipk (one per machine). Filename is deterministic:
    # demo-<machine>_1.0.0_x86_64.ipk.
    ipk=""
    if [[ -d "$src_dir" ]]; then
        ipk="$(find "$src_dir" -maxdepth 1 -name '*.ipk' -print -quit)"
    fi
    if [[ -n "$ipk" ]]; then
        cp "$ipk" "$dst_dir/${m}.ipk"
        log "  $m: copied $(basename "$ipk") → ${m}.ipk"
    else
        log "  $m: no .ipk built (no bazel_buildable components for this machine)"
        # Drop an empty .ipk-like marker so Puppet's opkg::check-ipk
        # can detect "intentional empty" vs "missing because forgot
        # to bazel build". Empty file => Puppet skips opkg install
        # gracefully (see theia::install).
        : > "$dst_dir/${m}.ipk"
    fi

    # executor.json (the supervisor tree, JSON-only since #380) +
    # machines.yaml (GUI manifest) come from the top-level
    # @rig//:executor_json and @rig//:machines_yaml genrules. Both
    # machines get the SAME files — the rig has one global executor
    # tree, and the GUI's machines.yaml lists every machine's endpoint.
    for f in executor.json machines.yaml; do
        if [[ -f "$BAZEL_RIG_DIR/$f" ]]; then
            cp "$BAZEL_RIG_DIR/$f" "$dst_dir/$f"
            log "  $m: copied $f"
        else
            log "  $m: WARNING: $BAZEL_RIG_DIR/$f missing — run 'bazel build @${RIG}//:executor_json @${RIG}//:machines_yaml'"
        fi
    done
done

# Make sure host log dirs exist (compose mounts them — empty dirs
# avoid permission surprises).
mkdir -p "$DEPLOY_DIR/logs/central" "$DEPLOY_DIR/logs/compute"

log "staged into $DEPLOY_DIR/.staging/"
log "Next: docker compose -f $DEPLOY_DIR/docker-compose.yml up"

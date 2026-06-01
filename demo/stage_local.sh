#!/usr/bin/env bash
# demo/stage_local.sh — lay out install/central for a local single-machine run
# of the NEW ara::exec supervisor (gen-app FC), WITHOUT Bazel pkg_install or
# Puppet.
#
# The machine dir mirrors the runtime filesystem shape the supervisor expects
# (executor.json's start_cmd is `bin/<ident>` relative to the supervisor's CWD):
#
#   install/central/
#     supervisor          — //platform/supervisor/main:supervisor (gen-app FC)
#     executor.json       — the supervisor tree (artheia executor emit --rig)
#     bin/<child>         — every start_cmd `bin/<x>` leaf, one per child
#     services-log        — the log[trace] collector (started separately)
#     netgraph.json       — addr→component-name map for the collector
#
# This rig is SINGLE-MACHINE (central hosts everything: all FCs + p1/p2/p3).
# The 2-machine central+compute layout lives in demo/manifest/zonal_rig.py;
# add a `stage compute ...` invocation against it when you want the split.
#
# Launch (from this script's output):
#   cd install/central
#   THEIA_SUPERVISOR_MANIFEST=./executor.json THEIA_ROOT_DIR=. ./supervisor
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

unset PYTHONPATH
export PATH="$REPO/.venv/bin:$PATH"

BB="$REPO/bazel-bin"
SUP="$BB/platform/supervisor/main/supervisor"

# --- build everything the stage needs (idempotent) -------------------------
echo "building supervisor + FCs + demo apps + collector ..."
bazel build \
    //platform/supervisor/main:supervisor \
    //services/sm/main:sm \
    //services/per/main:per \
    //services/ucm/main:ucm \
    //services/shwa/main:shwa \
    //services/log/main:log \
    //demo/Demo3WayP1/main:demo \
    //demo/Demo3WayP2/main:demo \
    //demo/Demo3WayP3/main:demo

# spec: "<bin-name>:<built-binary-path>" — bin-name is the executor.json
# start_cmd leaf (bin/<bin-name>); src is the bazel-built binary.
stage() {
    local machine="$1" rig="$2"; shift 2
    local dir="$REPO/install/$machine"
    rm -rf "$dir"
    mkdir -p "$dir/bin"

    # executor.json — the machine's supervisor tree.
    artheia executor emit demo.manifest.rig --rig "$rig" \
        --out "$dir/executor.json"

    # supervisor binary at the machine root.
    cp -f "$SUP" "$dir/supervisor"

    # bin/<child> for each requested binary.
    for spec in "$@"; do
        local name="${spec%%:*}" src="${spec#*:}"
        if [[ ! -x "$src" ]]; then
            echo "WARN: $src not built — skipping bin/$name" >&2
            continue
        fi
        cp -f "$src" "$dir/bin/$name"
    done
    echo "staged install/$machine/  ($# binaries)"
}

stage central CentralRig \
    "sm:$BB/services/sm/main/sm" \
    "per:$BB/services/per/main/per" \
    "ucm:$BB/services/ucm/main/ucm" \
    "shwa:$BB/services/shwa/main/shwa" \
    "p1:$BB/demo/Demo3WayP1/main/demo" \
    "p2:$BB/demo/Demo3WayP2/main/demo" \
    "p3:$BB/demo/Demo3WayP3/main/demo"

# services/log[trace] collector — the trace EGRESS service. NOT a supervised
# child (it's the trace hub); launched alongside the supervisor by whoever
# drives the run (e.g. an rf trace scenario / tdb). It needs the cluster
# netgraph.json for the src/dst addr→component-name rewrite.
LOG_BIN="$BB/services/log/main/log"
if [[ -x "$LOG_BIN" ]]; then
    cp -f "$LOG_BIN" "$REPO/install/central/services-log"
    artheia gen-netgraph -R system/system.art \
        --out "$REPO/install/central/netgraph.json"
    echo "staged install/central/services-log + netgraph.json"
else
    echo "WARN: $LOG_BIN not built — skipping collector stage"
fi

cat <<EOF

done. to run:
  cd install/central
  THEIA_SUPERVISOR_MANIFEST=./executor.json THEIA_ROOT_DIR=. ./supervisor
EOF

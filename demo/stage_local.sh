#!/usr/bin/env bash
# apps/stage_local.sh — lay out install/central for a local single-machine run
# of the ara::exec supervisor (gen-app FC).
#
# THIN WRAPPER around `theia stage-local`: the build + install/ population +
# setcap are owned by Puppet now (theia::local_install + theia::postinstall),
# the SAME code path a real deploy uses — bazel only builds, Puppet orchestrates.
# This script just adds the demo-specific netgraph emit + the run hint on top.
#
#   install/central/
#     supervisor     — //platform/supervisor/main:supervisor (cap_sys_nice)
#     bin/<child>    — every executor.json start_cmd `bin/<x>` leaf
#     executor.json  — the supervisor tree (artheia executor emit)
#     netgraph.json  — addr→component-name map for the log[trace] hub
#
# Launch (from this script's output):
#   cd install/central
#   THEIA_SUPERVISOR_MANIFEST=./executor.json THEIA_ROOT_DIR=. ./supervisor
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

unset PYTHONPATH
export PATH="$REPO/.venv/bin:$PATH"

# 1. Build + populate install/central/ + setcap — via Puppet (theia stage-local).
#    bazel builds; Puppet copies the binaries in and applies the cap contract.
"$REPO/theia" stage-local central

# 2. Demo-specific: cluster netgraph.json for the log[trace] hub's src/dst
#    addr→component-name rewrite. (Build-only artifact, not part of the supervised
#    tree — kept here rather than in the generic stage-local path.)
if [[ -x "$REPO/bazel-bin/services/log/main/log" ]]; then
    artheia gen-netgraph -R system/system.art \
        --out "$REPO/install/central/netgraph.json"
    echo "staged install/central/netgraph.json"
fi

cat <<EOF

done. to run:
  cd install/central
  THEIA_SUPERVISOR_MANIFEST=./executor.json THEIA_ROOT_DIR=. ./supervisor
EOF

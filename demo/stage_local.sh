#!/usr/bin/env bash
# demo/stage_local.sh — lay out install/central + install/compute for a
# local 2-supervisor run, WITHOUT Bazel pkg_install or Puppet.
#
# Each machine dir mirrors the runtime filesystem shape the supervisor
# expects:
#
#   install/<machine>/
#     supervisor          — the OTP-style supervisor (CMake-built)
#     executor.json       — that machine's tree (artheia executor emit --rig)
#     bin/<child>         — every start_cmd `bin/<x>` leaf, one per child
#
# Central runs sm/com/per/ucm + apps p1/p2; compute runs shwa + app p3.
# start_cmd is `bin/<ident>` relative to the supervisor's CWD, so we run
# the supervisor from inside install/<machine>/.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

unset PYTHONPATH
export PATH="$REPO/.venv/bin:$PATH"

SUP="$REPO/platform/supervisor/build/supervisor"
BZ="$REPO/bazel-bin/services"
APPS="$REPO/demo/build"

stage() {
    local machine="$1" rig="$2"; shift 2
    local dir="$REPO/install/$machine"
    rm -rf "$dir"
    mkdir -p "$dir/bin"

    # executor.json — the machine's whole standalone tree.
    artheia executor emit demo.manifest.rig --rig "$rig" \
        --out "$dir/executor.json"

    # supervisor binary at the machine root.
    cp -f "$SUP" "$dir/supervisor"

    # bin/<child> for each requested binary.
    for spec in "$@"; do
        local name="${spec%%:*}" src="${spec#*:}"
        cp -f "$src" "$dir/bin/$name"
    done
    echo "staged install/$machine/  ($# binaries)"
}

stage central CentralRig \
    "sm:$BZ/sm/main/sm" \
    "com:$BZ/com/main/com" \
    "per:$BZ/per/main/per" \
    "ucm:$BZ/ucm/main/ucm" \
    "p1:$APPS/p1" \
    "p2:$APPS/p2"

stage compute ComputeRig \
    "shwa:$BZ/shwa/main/shwa" \
    "p3:$APPS/p3"

echo "done."

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
#                           (includes bin/log — the log[trace] hub, now a
#                            supervised child, not a standalone sidecar)
#     netgraph.json       — addr→component-name map for the log[trace] hub
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

# Wipe the whole install/ tree first — this is a SINGLE-MACHINE local stage
# (rig.py is central-only), so a fresh install/ should contain only central/.
# Clearing the lot stops stale machine dirs (compute/, or an old etc//theia/
# layout) from lingering after the rig was trimmed.
rm -rf "$REPO/install"
mkdir -p "$REPO/install"

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

    # File capabilities (mirror deploy/puppet provisioning.pp):
    #   - supervisor: cap_sys_nice — apply SCHED_FIFO/RR + affinity on FC node
    #     threads (THEIA_NODE_CFG); without it those soft-fail EPERM.
    #   - gateway (if staged): cap_net_raw,cap_net_admin — raw bus sockets.
    # setcap needs root; cp clears caps, so re-apply every stage. Skipped (with
    # a hint) when sudo isn't available — affinity still works, rt-sched won't.
    if command -v setcap >/dev/null 2>&1; then
        if sudo -n true 2>/dev/null; then
            sudo setcap cap_sys_nice+eip "$dir/supervisor" || true
            [[ -x "$dir/gateway" ]] && \
                sudo setcap cap_net_raw,cap_net_admin+eip "$dir/gateway" || true
        else
            echo "NOTE: no passwordless sudo — skipping setcap; rt-sched on node" >&2
            echo "      threads will soft-fail EPERM. Run: sudo setcap cap_sys_nice+eip $dir/supervisor" >&2
        fi
    fi

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
    "log:$BB/services/log/main/log" \
    "sm:$BB/services/sm/main/sm" \
    "per:$BB/services/per/main/per" \
    "ucm:$BB/services/ucm/main/ucm" \
    "shwa:$BB/services/shwa/main/shwa" \
    "p1:$BB/demo/Demo3WayP1/main/demo" \
    "p2:$BB/demo/Demo3WayP2/main/demo" \
    "p3:$BB/demo/Demo3WayP3/main/demo"

# services/log[trace] — the ring-buffer trace hub (TraceStreamPump 0x80010013
# + TraceCtl 0x80010014). It is now a SUPERVISED CHILD (bin/log, staged above):
# the rig manifest lists it under host_svc_sup, so the supervisor forks it like
# any other FC and restart-manages it. tdb logcat / artheia.observer Subscribe to
# TraceCtl; per-node Tracer records egress to the pump. (Previously staged as a
# standalone `services-log` sidecar "launched alongside" — but nothing launched
# it, so logcat had no listener. Now it's in the tree.) It still needs the
# cluster netgraph.json for the src/dst addr→component-name rewrite.
if [[ -x "$BB/services/log/main/log" ]]; then
    artheia gen-netgraph -R system/system.art \
        --out "$REPO/install/central/netgraph.json"
    echo "staged install/central/netgraph.json"
else
    echo "WARN: services/log not built — skipping netgraph stage" >&2
fi

cat <<EOF

done. to run:
  cd install/central
  THEIA_SUPERVISOR_MANIFEST=./executor.json THEIA_ROOT_DIR=. ./supervisor
EOF

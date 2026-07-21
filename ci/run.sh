#!/usr/bin/env bash
# ci/run.sh — the END-USER-FLOW harness. One entry point, identical locally and
# in CI:
#
#   ./ci/run.sh                 # all scenarios
#   ./ci/run.sh s1 s4           # a subset
#   NO_LIVE=1 ./ci/run.sh       # skip the runtime phase (no TIPC / no sudo)
#
# Every scenario scaffolds a FRESH workspace under ci/.work/ via the real user
# entry points (`theia init`, `artheia gen-fc`, bazel against @pero_theia),
# grafts the committed seed (ci/seeds/, ci/demo/ — USER-side code only), drives
# the full toolchain, and asserts the RUNTIME behaviour with Robot Framework
# (ci/test/). Nothing generated is ever committed; nothing in ci/ may be
# imported by framework code. See ci/README.md for the contract.
#
# Scenarios:
#   s1  ws-bare      init --kind ws → placeholder app → build → install → live
#                    ping + the enabled-override regression (identity seams)
#   s2  ws-demo      init --kind ws --name apps → graft the Demo3Way seed
#                    (multi-composition, statem, connects) → build → live checks
#   s3  ws-services  init --with-services → manifest the full FC tree → install
#                    (builds every FC against @pero_theia) → live tree up
#   s4  pkg          init --kind package → graft sensor seed → package+tester
#                    gen → build → live: the scaffold's own robot probe +
#                    params-alias regression
#   s5  pkg-consume  sensor + filter packages consumed by a fresh workspace via
#                    bazel-module overrides; cross-package connect; live: data
#                    FLOWS producer → consumer (GetStats.received grows)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CI="$ROOT/ci"
WORK="${CI_WORK:-$CI/.work}"
export THEIA_ROOT="$ROOT"

# Prefer the repo venv (dev box); fall back to PATH (CI: pip-installed artheia,
# bazelisk-provided bazel).
if [ -d "$ROOT/.venv/bin" ]; then export PATH="$ROOT/.venv/bin:$PATH"; fi
THEIA="${THEIA:-python3 $ROOT/tools/theia.py}"
ARTHEIA="${ARTHEIA:-artheia}"

# ── live-phase gate ─────────────────────────────────────────────────────────
# The runtime phase needs the TIPC module (and sudo for the supervisor setcap).
# NO_LIVE=1 forces build-only; otherwise auto-detect and try to load tipc.
live_ok() {
    [ "${NO_LIVE:-0}" = "1" ] && return 1
    [ -d /sys/module/tipc ] && return 0
    sudo -n modprobe tipc 2>/dev/null && return 0
    echo "  (tipc unavailable → live phase skipped; set up tipc or NO_LIVE=1 to silence)"
    return 1
}

# ── helpers ─────────────────────────────────────────────────────────────────
log()  { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }
fail() { printf '\033[31mFAIL: %s\033[0m\n' "$*"; exit 1; }

quiet() {  # $1=step-name, rest=command — capture output, dump tail on failure
    local step="$1"; shift
    local logf="$WORK/logs/${step// /_}.log"
    mkdir -p "$WORK/logs"
    if ! "$@" >"$logf" 2>&1; then
        printf '\033[31m-- %s failed; last 60 lines of %s --\033[0m\n' "$step" "$logf"
        tail -60 "$logf"
        fail "$step"
    fi
}

fresh_ws() {  # $1=dir
    rm -rf "$1"; mkdir -p "$1"
}

stop_ws() {   # $1=ws dir — best-effort stop of a started supervisor
    ( cd "$1" && $THEIA stop >/dev/null 2>&1 ) || true
}

start_ws() {  # $1=ws dir $2=target — manifest+install+start; caller stops
    ( cd "$1" \
      && $THEIA manifest "$2" >/dev/null \
      && $THEIA install  "$2" >/dev/null \
      && $THEIA start ) \
      || fail "start_ws $1 $2"
    sleep 2
}

robot_run() { # $1=out-subdir, rest = robot args
    local out="$WORK/robot/$1"; shift
    mkdir -p "$out"
    python3 -m robot --outputdir "$out" --consolewidth 100 "$@" \
        || fail "robot suite failed (report: $out/log.html)"
    # a suite where nothing PASSED (all skipped) must not count as green.
    grep -q 'pass="0" fail="0"' "$out/output.xml" \
        && fail "robot suite ran ZERO tests (all skipped?) — $out/log.html"
}

tipc_bound() {  # $1=hex type (e.g. 0xC1010001) → 0 if bound
    tipc nametable show 2>/dev/null | awk -v t="$(( $1 ))" '$1==t{f=1} END{exit !f}'
}

# ── s1: bare workspace + identity-seam regression ───────────────────────────
s1() {
    local ws="$WORK/s1-ws"; fresh_ws "$ws"; cd "$ws"
    log "s1: theia init --kind ws (placeholder app)"
    $THEIA init --kind ws --name pilot >/dev/null 2>&1
    $ARTHEIA gen-fc system/pilot/component.art --out apps --proto-out proto >/dev/null
    $ARTHEIA gen-manifest system/pilot/component.art manifest/pilot/manifest.py >/dev/null

    log "s1: negative — gen-fc on an EMPTIED cluster must refuse (exit 2)"
    local t="$WORK/s1-empty"; fresh_ws "$t"; ( cd "$t" && $THEIA init --kind ws --name pilot >/dev/null 2>&1 \
        && printf 'package system.pilot\n\ncluster Applications { }\n' > system/pilot/component.art \
        && ! $ARTHEIA gen-fc system/pilot/component.art --out apps --proto-out proto >/dev/null 2>&1 ) \
        || fail "empty-cluster gen-fc did not refuse"

    log "s1: scaffold pins — .bazelversion matches framework, -c opt present"
    diff -q "$ws/.bazelversion" "$ROOT/.bazelversion" >/dev/null || fail ".bazelversion drift"
    grep -q '^build -c opt' "$ws/.bazelrc" || fail "scaffold .bazelrc missing -c opt"

    log "s1: build //apps/... against @pero_theia"
    ( cd "$ws" && quiet "s1 build" bazel build //apps/... )

    if live_ok; then
        log "s1: live — start, ping, then the enabled-override regression"
        start_ws "$ws" pilot
        robot_run s1 --variable WS:"$ws" --variable NODE:PilotNode \
                  --variable OP:Ping --variable TIPC:0xD0010001 "$CI/test/s1_ws.robot"
        stop_ws "$ws"
        # Regression (identity seams): a deploy/config override must REACH the
        # node — enabled=false ⇒ its TIPC name must NOT bind.
        mkdir -p "$ws/deploy/config/central"
        # the override file is named by the PROCESS (the cluster-member ident,
        # `app`), NOT the workspace/package name — the very seam this guards.
        printf '{"nodes": {"pilot": {"enabled": false}}}\n' > "$ws/deploy/config/central/app.json"
        start_ws "$ws" pilot
        tipc_bound 0xD0010001 && { stop_ws "$ws"; fail "enabled=false override NOT read (identity seam regressed)"; }
        stop_ws "$ws"
        echo "  override read ✓ (node held disabled)"
    fi
    echo "s1 PASS"
}

# ── s2: the demo seed (multi-composition, statem, connects) ─────────────────
s2() {
    local ws="$WORK/s2-demo"; fresh_ws "$ws"; cd "$ws"
    log "s2: theia init --kind ws --name apps + graft the Demo3Way seed"
    $THEIA init --kind ws --name apps >/dev/null 2>&1
    cp "$CI"/demo/system-apps/*.art "$ws/system/apps/"
    # write-once impl bodies pre-placed → gen-fc skips them (the user story).
    ( cd "$CI/demo/impl" && find . -type f ) | while read -r f; do
        mkdir -p "$ws/apps/$(dirname "$f")"; cp "$CI/demo/impl/$f" "$ws/apps/$f"
    done
    $ARTHEIA gen-fc system/apps/component.art --out apps --proto-out proto >/dev/null
    $ARTHEIA gen-manifest system/apps/component.art manifest/apps/manifest.py >/dev/null

    # Graft the app-side trace-decoder plugin seed (the hand-written example of
    # the pluggable decoder ABI — supervisor-gui/rtdb dlopen it) + the
    # protoc-cpp binding it needs, appended to the generated proto BUILD.
    cp -r "$CI/demo/trace" "$ws/trace"
    cat "$CI/demo/proto-apps-cpp.BUILD.frag" >> "$ws/proto/system/apps/BUILD.bazel"

    # Declare the rig repo (@rig_apps) so the .ipk deploy path — rules/rig.bzl
    # + dist_ipk — is exercised from a consuming workspace (gen_chain stage 8).
    cat >> "$ws/MODULE.bazel" <<'EOF'
rig_ext = use_extension("@pero_theia//rules:rig.bzl", "rig_ext")
rig_ext.declare(
    name = "rig_apps",
    rig_module = "manifest.apps.rig",
    rig_attr = "RIG",
)
use_repo(rig_ext, "rig_apps")
EOF

    log "s2: gen-chain — every artheia pipeline stage against the scaffold"
    robot_run s2-gen-chain --variable WORKSPACE:"$ws" \
              --variable WORKDIR:"$WORK/s2-gen-chain" "$CI/test/gen_chain.robot"

    log "s2: build all demo compositions (incl. the statem main + trace plugin)"
    ( cd "$ws" && quiet "s2 build" bazel build //apps/... //trace/... )

    if live_ok; then
        log "s2: live — 4 processes up, counter reaches 50"
        start_ws "$ws" apps
        robot_run s2 --variable WS:"$ws" "$CI/test/s2_demo.robot"
        stop_ws "$ws"
    fi
    echo "s2 PASS"
}

# ── s3: --with-services (the full FC tree builds + manifests) ───────────────
s3() {
    local ws="$WORK/s3-services"; fresh_ws "$ws"; cd "$ws"
    log "s3: theia init --kind ws --with-services"
    $THEIA init --kind ws --name svc --with-services >/dev/null 2>&1
    $ARTHEIA check-addresses system/system.art >/dev/null || fail "s3 parse"
    $ARTHEIA gen-fc system/svc/component.art --out apps --proto-out proto >/dev/null
    $ARTHEIA gen-manifest system/svc/component.art manifest/svc/manifest.py >/dev/null
    ( cd "$ws" && $THEIA manifest svc >/dev/null ) || fail "s3 manifest"
    ls "$ws"/dist/manifest/central/config/sm.json >/dev/null 2>&1 || fail "s3: FC configs not staged"

    log "s3: install (builds every service FC against @pero_theia)"
    ( cd "$ws" && quiet "s3 install" $THEIA install svc )

    if live_ok; then
        log "s3: live — service tree up (per/nm held: no etcd/netadmin here)"
        mkdir -p "$ws/deploy/config/central"
        printf '{"children":[{"name":"services_sup","children":[{"name":"per","run_on_start":false},{"name":"nm","run_on_start":false}]}]}\n' \
            > "$ws/deploy/config/central/executor.json"
        ( cd "$ws" && $THEIA install svc >/dev/null 2>&1 )
        start_ws "$ws" svc
        robot_run s3 --variable WS:"$ws" "$CI/test/s3_services.robot"
        stop_ws "$ws"
    fi
    echo "s3 PASS"
}

# ── s4: a package repo end to end (its own probe is the runtime check) ──────
s4() {
    local ws="$WORK/s4-pkg"; fresh_ws "$ws"; cd "$ws"
    log "s4: theia init --kind package + sensor seed"
    $THEIA init --kind package --name sensor >/dev/null 2>&1
    cp "$CI/seeds/pkg-sensor/package.art" "$ws/system/sensor/package.art"
    mkdir -p "$ws/src/impl"; cp "$CI"/seeds/pkg-sensor/impl/* "$ws/src/impl/"
    $ARTHEIA gen-fc-lib system/sensor/package.art --out src --proto-out proto --ns ara::sensor >/dev/null
    $ARTHEIA gen-fc system/sensor_tester/component.art --out apps --proto-out proto >/dev/null
    $ARTHEIA gen-manifest system/sensor_tester/component.art manifest/sensor/manifest.py >/dev/null

    log "s4: params-alias regression — manifest carries BOTH identity keys"
    grep -q "'sensor'" "$ws/manifest/sensor/manifest.py" || fail "prototype params key missing"
    grep -q "'sensor_ctrl'" "$ws/manifest/sensor/manifest.py" || fail "type-snake params ALIAS missing (imported-node seam regressed)"

    log "s4: build the tester (links //src/lib + //src/impl)"
    ( cd "$ws" && quiet "s4 build" bazel build //apps/... )

    if live_ok; then
        log "s4: live — the package's own scaffold-shipped robot probe"
        start_ws "$ws" rig
        robot_run s4 "$ws/test/sensor.robot"
        stop_ws "$ws"
    fi
    echo "s4 PASS"
}

# ── s5: two packages consumed by a workspace; data flows across the connect ─
s5() {
    local pk="$WORK/s5-pkgs"; fresh_ws "$pk"
    log "s5: scaffold sensor + filter package repos"
    for p in sensor filter; do
        mkdir -p "$pk/$p"; ( cd "$pk/$p" && $THEIA init --kind package --name "$p" >/dev/null 2>&1 )
        cp "$CI/seeds/pkg-$p/package.art" "$pk/$p/system/$p/package.art"
        mkdir -p "$pk/$p/src/impl"; cp "$CI"/seeds/pkg-"$p"/impl/* "$pk/$p/src/impl/"
    done
    # filter imports system.sensor → resolvable via the documented sibling link,
    # and — bzlmod repo visibility is PER MODULE — filter's own MODULE.bazel must
    # bazel_dep the sensor module for @sensor// to be visible from @filter+
    # (the root consumer's local_path_override then maps it; filter's own
    # override applies only when filter builds standalone).
    ln -sfn ../../sensor/system/sensor "$pk/filter/system/sensor"
    python3 - "$pk/filter/MODULE.bazel" <<'MPY' 
import sys
p = sys.argv[1]
s = open(p).read()
anchor = 'local_path_override(module_name = "pero_theia"'
i = s.find(chr(10), s.index(anchor)) + 1
add = ('bazel_dep(name = "sensor", version = "0.1.0")' + chr(10)
       + 'local_path_override(module_name = "sensor", path = "../sensor")' + chr(10))
open(p, "w").write(s[:i] + add + s[i:])
MPY
    ( cd "$pk/sensor" && $ARTHEIA gen-fc-lib system/sensor/package.art --out src --proto-out proto --ns ara::sensor >/dev/null )
    ( cd "$pk/filter" && $ARTHEIA gen-fc-lib system/filter/package.art --out src --proto-out proto --ns ara::filter >/dev/null )

    local ws="$WORK/s5-ws"; fresh_ws "$ws"; cd "$ws"
    log "s5: consumer workspace composing both packages (bazel modules)"
    $THEIA init --kind ws --name pipeline >/dev/null 2>&1
    ln -sfn "$pk/sensor/system/sensor" "$ws/system/sensor"
    ln -sfn "$pk/filter/system/filter" "$ws/system/filter"
    cp "$CI/seeds/consumer/component.art" "$ws/system/pipeline/component.art"
    cp "$CI/seeds/consumer/package.art"   "$ws/system/pipeline/package.art"
    python3 - "$ws" "$pk" <<'PY'
import sys
ws, pk = sys.argv[1], sys.argv[2]
p = ws + "/MODULE.bazel"
s = open(p).read()
anchor = 'local_path_override(module_name = "pero_theia"'
i = s.index("\n", s.index(anchor)) + 1
add = ('bazel_dep(name = "sensor", version = "0.1.0")\n'
       f'local_path_override(module_name = "sensor", path = "{pk}/sensor")\n'
       'bazel_dep(name = "filter", version = "0.1.0")\n'
       f'local_path_override(module_name = "filter", path = "{pk}/filter")\n')
open(p, "w").write(s[:i] + add + s[i:])
PY
    $ARTHEIA gen-fc system/pipeline/component.art --out apps --proto-out proto >/dev/null
    $ARTHEIA gen-manifest system/pipeline/component.art manifest/pipeline/manifest.py >/dev/null
    grep -q '@sensor//src/lib' "$ws"/apps/*/main/BUILD.bazel || fail "module-qualified label missing"

    log "s5: build the composed app (@sensor// + @filter//)"
    ( cd "$ws" && quiet "s5 build" bazel build //apps/... )

    if live_ok; then
        log "s5: live — samples FLOW sensor → filter across the connect"
        start_ws "$ws" pipeline
        robot_run s5 --variable WS:"$ws" "$CI/test/s5_pipeline.robot"
        stop_ws "$ws"
    fi
    echo "s5 PASS"
}

# ── preflight: sweep leftovers from previous harness runs ───────────────────
# A crashed/interrupted run can orphan a supervisor (argv "./supervisor", cwd in
# ci/.work) that keeps 0x80020001 bound and blocks every later start (the
# collision guard). Stop via pidfiles first, then kill by cwd as a fallback.
preflight() {
    local d pidf pid
    for d in "$WORK"/*/; do
        pidf="$d/install/central/supervisor.pid"
        [ -f "$pidf" ] && ( cd "$d" && $THEIA stop >/dev/null 2>&1 ) || true
    done
    for pid in $(pgrep -x supervisor 2>/dev/null || true); do
        case "$(readlink -f /proc/$pid/cwd 2>/dev/null)" in
            "$WORK"/*) kill "$pid" 2>/dev/null || true ;;
        esac
    done
    sleep 1
}

# ── main ────────────────────────────────────────────────────────────────────
SCENARIOS=("$@")
[ ${#SCENARIOS[@]} -eq 0 ] && SCENARIOS=(s1 s2 s3 s4 s5)
mkdir -p "$WORK"
preflight
RESULT=0
for s in "${SCENARIOS[@]}"; do
    if "$s"; then :; else RESULT=1; echo "$s FAILED"; fi
done
exit $RESULT

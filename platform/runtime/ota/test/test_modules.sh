#!/bin/sh
# test_modules.sh — hermetic tests for the Mender update modules (theia-swp,
# theia-app, theia-release) and the Mender→UCM state script.
#
# Runs as a NORMAL USER, on the host or in the build container; needs only
# sh, tar, gzip, pgrep and python3 (python3 only for the theia-app cases — the
# module itself needs it). Every case drives a module through its Mender states
# (`<module> <STATE> <payload-tree>`) against a throwaway THEIA_ROOT under
# mktemp; nothing outside that temp dir is touched.
#
#   sh platform/runtime/ota/test/test_modules.sh          # all cases
#   sh platform/runtime/ota/test/test_modules.sh -v       # + module output
#
# A stub $THEIA_ROOT/bin/tdb (exit 0) is ALWAYS installed: it makes the modules'
# supervisor reload a no-op, so the tests never signal a real supervisor that
# may be running on this machine. The commit health gate is exercised with a
# stand-in `supervisor` binary (a copy of sleep) under the temp root.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
OTA="$(cd "$HERE/.." && pwd)"
SWP="$OTA/modules/theia-swp"
APP="$OTA/modules/theia-app"
REL="$OTA/modules/theia-release"
HOOK="$OTA/state-scripts/ArtifactInstall_Leave_01_ucm-request"

VERBOSE=0; [ "${1:-}" = "-v" ] && VERBOSE=1
TMP="$(mktemp -d "${TMPDIR:-/tmp}/theia-ota-test.XXXXXX")"
SUP_PIDS=""
cleanup() {
    for p in $SUP_PIDS; do kill "$p" 2>/dev/null; done
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

PASS=0; FAIL=0
OUT="$TMP/out.txt"; RC=0

ok()   { PASS=$((PASS + 1)); echo "ok   - $1"; }
nok()  { FAIL=$((FAIL + 1)); echo "FAIL - $1"; sed 's/^/       | /' "$OUT"; }
# check <description> <condition…>: the condition is a command.
check() { d="$1"; shift; if "$@"; then ok "$d"; else nok "$d"; fi; }

# run <root> <module> <state> [tree]  → RC + $OUT (stdout+stderr)
run() {
    _root="$1"; _mod="$2"; _state="$3"; _tree="${4:-}"
    THEIA_ROOT="$_root" sh "$_mod" "$_state" "$_tree" >"$OUT" 2>&1; RC=$?
    [ "$VERBOSE" = 1 ] && sed 's/^/       > /' "$OUT"
    return 0
}
rc_is()     { [ "$RC" -eq "$1" ]; }
rc_fail()   { [ "$RC" -ne 0 ]; }
said()      { grep -q -- "$1" "$OUT"; }
cur_is()    { [ "$(readlink "$1/current" 2>/dev/null)" = "$2" ]; }
refused()   { [ "$RC" -ne 0 ] && grep -q "REFUSED" "$OUT"; }

# ---- fixtures ----------------------------------------------------------------
# A provisioned rig: releases/runtime-1.0 is current; tdb is a no-op stub.
mkroot() {
    r="$1"
    mkdir -p "$r/releases/runtime-1.0/bin" "$r/releases/runtime-1.0/lib" \
             "$r/releases/runtime-1.0/config" "$r/bin" "$r/config"
    echo live > "$r/releases/runtime-1.0/bin/com"
    echo '{"name":"root","nodes":[]}' > "$r/releases/runtime-1.0/config/executor.json"
    echo '{"name":"root","nodes":[]}' > "$r/config/executor.json"
    ln -s releases/runtime-1.0 "$r/current"
    printf '#!/bin/sh\nexit 0\n' > "$r/bin/tdb"; chmod 755 "$r/bin/tdb"
}
# The good SWP source tree (what `theia release-swp` tars).
mkswpsrc() {
    s="$1"
    mkdir -p "$s/bin" "$s/manifest/master"
    echo fc > "$s/bin/demo_fc"
    echo '{"name":"root","nodes":[{"name":"demo_fc"}]}' > "$s/manifest/master/executor.json"
    echo '{}' > "$s/manifest/machines.json"
}
# mktree <tree> <version.txt content> <tarball-to-copy> [name-in-payload]
mktree() {
    mkdir -p "$1/files"
    printf '%s\n' "$2" > "$1/files/version.txt"
    cp "$3" "$1/files/${4:-payload.tar.gz}"
}

W="$TMP/fix"; mkdir -p "$W"
mkswpsrc "$W/swp"
( cd "$W/swp" && tar -czf "$W/swp-good.tar.gz" . )
# a release tree for theia-release
mkdir -p "$W/rel/bin"; echo v2 > "$W/rel/bin/com"
( cd "$W/rel" && tar -czf "$W/rel-good.tar.gz" . )
# hostile archives (built over the good SWP tree so only ONE thing is wrong)
cp -r "$W/swp" "$W/sym";  ln -s /etc/passwd "$W/sym/bin/link"
( cd "$W/sym" && tar -czf "$W/bad-symlink.tar.gz" . )
cp -r "$W/swp" "$W/hard"; ln "$W/hard/bin/demo_fc" "$W/hard/bin/hardlink"
( cd "$W/hard" && tar -czf "$W/bad-hardlink.tar.gz" . )
echo abs > "$W/abs-member.txt"
( cd "$W/swp" && tar -czPf "$W/bad-absolute.tar.gz" . "$W/abs-member.txt" ) 2>/dev/null
mkdir -p "$W/dd/inner"; echo esc > "$W/dd/evil.txt"
( cd "$W/dd/inner" && tar -czPf "$W/bad-dotdot.tar.gz" -C "$W/swp" . -C "$W/dd/inner" ../evil.txt ) 2>/dev/null
cp -r "$W/swp" "$W/ff"; mkfifo "$W/ff/bin/pipe"
( cd "$W/ff" && tar -czf "$W/bad-fifo.tar.gz" . )

# sanity: the hostile fixtures really contain what they claim
check "fixture: absolute-path archive has a /-member" \
      sh -c "tar -tzf '$W/bad-absolute.tar.gz' 2>/dev/null | grep -q '^/'"
check "fixture: dotdot archive has a ../ member" \
      sh -c "tar -tzf '$W/bad-dotdot.tar.gz' 2>/dev/null | grep -q '^\.\./'"

# ---- the three helper blocks are identical -----------------------------------
block() { sed -n '/^# ---- hardening: untrusted-input validation/,/^# ---- end hardening block/p' "$1"; }
block "$SWP" > "$TMP/b-swp"; block "$APP" > "$TMP/b-app"; block "$REL" > "$TMP/b-rel"
: > "$OUT"
check "helper block present in theia-swp" test -s "$TMP/b-swp"
check "helper block identical: theia-swp == theia-app" cmp -s "$TMP/b-swp" "$TMP/b-app"
check "helper block identical: theia-swp == theia-release" cmp -s "$TMP/b-swp" "$TMP/b-rel"

# ==============================================================================
# theia-swp
# ==============================================================================
R="$TMP/swp1/opt-theia"; mkroot "$R"
mktree "$TMP/swp1/tree" "demo-1.0.0-jammy" "$W/swp-good.tar.gz"
run "$R" "$SWP" ArtifactInstall "$TMP/swp1/tree"
check "swp: good artifact installs (rc 0)" rc_is 0
check "swp: current → releases/demo-1.0.0-jammy" cur_is "$R" "releases/demo-1.0.0-jammy"
check "swp: SWP FC binary staged" test -f "$R/releases/demo-1.0.0-jammy/bin/demo_fc"
check "swp: services baseline carried over" test -f "$R/releases/demo-1.0.0-jammy/bin/com"
check "swp: previous → the outgoing release" \
      test "$(readlink "$R/previous")" = "releases/runtime-1.0"

# re-install of the RUNNING release is refused and the live tree survives
echo marker > "$R/releases/demo-1.0.0-jammy/LIVE"
run "$R" "$SWP" ArtifactInstall "$TMP/swp1/tree"
check "swp: version == current is refused" refused
check "swp: ... message names the running release" said "currently running release"
check "swp: ... the live tree was not deleted" test -f "$R/releases/demo-1.0.0-jammy/LIVE"
check "swp: ... current unchanged" cur_is "$R" "releases/demo-1.0.0-jammy"

# hostile version.txt values — nothing outside the temp root is touched
# shellcheck disable=SC2016  # the literal text '$(id)' IS the hostile input
for bad in '../..' '..' '.' 'a/b' '/etc' '-rf' 'a b' 'a;b' 'x$(id)' \
           'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'; do
    C="$TMP/swpbad"; rm -rf "$C"; R="$C/opt-theia"; mkroot "$R"
    echo keep > "$C/canary"; mkdir -p "$C/sibling"; echo keep > "$C/sibling/file"
    mktree "$C/tree" "$bad" "$W/swp-good.tar.gz"
    run "$R" "$SWP" ArtifactInstall "$C/tree"
    check "swp: version.txt '$bad' is refused" refused
    check "swp: ... '$bad': canary outside the root intact" \
          sh -c "test -f '$C/canary' && test -f '$C/sibling/file' && test -d '$R/bin'"
    check "swp: ... '$bad': current unchanged, live tree intact" \
          sh -c "test \"\$(readlink '$R/current')\" = releases/runtime-1.0 && test -f '$R/releases/runtime-1.0/bin/com'"
    check "swp: ... '$bad': no new release dir" \
          sh -c "test \"\$(ls '$R/releases')\" = runtime-1.0"
done

# hostile archives
for kind in symlink hardlink absolute dotdot fifo; do
    C="$TMP/swptar-$kind"; R="$C/opt-theia"; mkroot "$R"
    mktree "$C/tree" "demo-2.0.0" "$W/bad-$kind.tar.gz"
    run "$R" "$SWP" ArtifactInstall "$C/tree"
    check "swp: archive with a $kind member is refused" refused
    check "swp: ... $kind: current unchanged, nothing staged" \
          sh -c "test \"\$(readlink '$R/current')\" = releases/runtime-1.0 && test ! -e '$R/releases/demo-2.0.0'"
done
check "swp: the ../ member was not written next to a work dir" test ! -e "${TMPDIR:-/tmp}/evil.txt"

# commit health gate
R="$TMP/swp1/opt-theia"
run_commit() {   # $1 = THEIA_OTA_HEALTH_WAIT_S
    THEIA_ROOT="$R" THEIA_OTA_HEALTH_WAIT_S="$1" THEIA_OTA_HEALTH_STABLE=2 \
        sh "$SWP" ArtifactCommit "$TMP/swp1/tree" >"$OUT" 2>&1; RC=$?
    [ "$VERBOSE" = 1 ] && sed 's/^/       > /' "$OUT"
    return 0
}
run_commit 2
check "swp: commit FAILS when the health signal is absent" rc_fail
check "swp: ... and says so" said "COMMIT FAILED"
cp "$(command -v sleep)" "$R/bin/supervisor"
"$R/bin/supervisor" 120 &
SUP_PIDS="$SUP_PIDS $!"
run_commit 10
check "swp: commit PASSES with a stable supervisor on the new release" rc_is 0
check "swp: ... and reports health OK" said "health OK"
kill "$!" 2>/dev/null; wait "$!" 2>/dev/null
run_commit 2
check "swp: commit fails again once the supervisor is gone" rc_fail
run_commit 0
check "swp: THEIA_OTA_HEALTH_WAIT_S=0 disables the gate (lab opt-out)" rc_is 0
check "swp: ... with a warning" said "DISABLED"

# rollback (fresh root: the refused re-install above re-snapshotted `previous`
# to the running release, exactly so that a rollback after a refusal is a no-op)
run "$R" "$SWP" ArtifactRollback "$TMP/swp1/tree"
check "swp: rollback after the refused re-install keeps the running release" \
      cur_is "$R" "releases/demo-1.0.0-jammy"
R="$TMP/swp2/opt-theia"; mkroot "$R"
mktree "$TMP/swp2/tree" "demo-1.0.0-jammy" "$W/swp-good.tar.gz"
run "$R" "$SWP" ArtifactInstall "$TMP/swp2/tree"
run "$R" "$SWP" ArtifactRollback "$TMP/swp2/tree"
check "swp: rollback with a previous succeeds" rc_is 0
check "swp: ... current → releases/runtime-1.0" cur_is "$R" "releases/runtime-1.0"
R="$TMP/swp-noprev/opt-theia"; mkroot "$R"
run "$R" "$SWP" ArtifactRollback ""
check "swp: rollback WITHOUT previous fails" rc_fail
check "swp: ... with a clear message" said "no previous release to roll back to"

# ------------------------------------------------------------------------------
# G1 app-process commit gate (spec hil-app-ota G1). Each case: a rig with a
# stable stand-in supervisor, app demo-1.0 installed, then demo-1.1 installed
# (current → demo-1.1, previous → demo-1.0); ArtifactCommit runs the gate; a
# failing gate is followed by ArtifactRollback, which must put demo-1.0 back.
# Case names "G1.<id>" are stable: release notes cite them.
# ------------------------------------------------------------------------------
SLEEP="$(command -v sleep)"
APPJ='{"name": "demo", "processes": [{"name": "demo_node", "exe": "demo_fc"}]}'
mkappswp() {   # $1 = src dir, $2 = app.json ("" = none), $3 = health hook body ("" = none)
    s="$1"; mkswpsrc "$s"; rm -f "$s/bin/demo_fc"; cp "$SLEEP" "$s/bin/demo_fc"; chmod 755 "$s/bin/demo_fc"
    [ -n "$2" ] && printf '%s\n' "$2" > "$s/app.json"
    if [ -n "${3:-}" ]; then printf '#!/bin/sh\n%s\n' "$3" > "$s/health"; chmod 755 "$s/health"; fi
    mkdir -p "$s/lib"; echo marker > "$s/lib/libswp_marker.so"
    ( cd "$s" && tar -czf "$s.tar.gz" . )
}
mkappswp "$W/app"           "$APPJ"
mkappswp "$W/app-noprocs"   ""
mkappswp "$W/app-hookfail"  "$APPJ" 'echo "no MavLink heartbeat"; exit 1'
mkappswp "$W/app-hookslow"  "$APPJ" 'echo "waiting"; sleep 30; exit 0'
mkappswp "$W/app-hookok"    "$APPJ" \
    '[ -f "$LD_LIBRARY_PATH/libswp_marker.so" ] || { echo "no SWP lib on LD_LIBRARY_PATH=$LD_LIBRARY_PATH"; exit 1; }
echo "starting"; echo "lib ok: LD_LIBRARY_PATH=$LD_LIBRARY_PATH cwd=$(pwd)"'
mkappswp "$W/app-timing"    '{"name": "demo", "gate": {"wait_s": 4, "stable_s": 1, "hook_timeout_s": 3},
  "processes": [{"name": "demo_node", "exe": "demo_fc"}]}'
mkappswp "$W/app-badname"   '{"name": "demo", "processes": [{"name": "x", "exe": "../../bin/sh"}]}'

LOOP=""
g1_setup() {   # $1 = case id, $2 = the demo-1.1 SWP tarball
    C="$TMP/g1-$1"; R="$C/opt-theia"; mkroot "$R"
    cp "$SLEEP" "$R/bin/supervisor"; "$R/bin/supervisor" 300 & SUP_PIDS="$SUP_PIDS $!"
    mktree "$C/t10" "demo-1.0" "$W/app.tar.gz"; run "$R" "$SWP" ArtifactInstall "$C/t10"
    mktree "$C/t11" "demo-1.1" "$2";            run "$R" "$SWP" ArtifactInstall "$C/t11"
}
g1_app() {     # start the app process the way the supervisor does: from current/bin
    "$R/current/bin/demo_fc" 300 & SUP_PIDS="$SUP_PIDS $!"
}
G1T="THEIA_OTA_HEALTH_STABLE=2 THEIA_OTA_HEALTH_WAIT_S=6 THEIA_OTA_APP_STABLE_S=2 THEIA_OTA_HOOK_TIMEOUT_S=2"
g1_commit() {  # [env args] — default: the fast test timings $G1T (later VAR=v wins)
    # shellcheck disable=SC2086  # $G1T is a word list on purpose
    [ $# -gt 0 ] || set -- $G1T
    env "$@" THEIA_ROOT="$R" sh "$SWP" ArtifactCommit "$C/t11" >"$OUT" 2>&1; RC=$?
    [ "$VERBOSE" = 1 ] && sed 's/^/       > /' "$OUT"
    return 0
}
g1_rolls_back() {   # $1 = case id: commit failed AND the rollback restores demo-1.0
    check "G1.$1: commit FAILS (rc != 0)" rc_fail
    cp "$OUT" "$C/commit.out"
    run "$R" "$SWP" ArtifactRollback "$C/t11"
    check "G1.$1: ArtifactRollback → current = releases/demo-1.0 (previous app)" \
          sh -c "test $RC -eq 0 && test \"\$(readlink '$R/current')\" = releases/demo-1.0"
    cp "$C/commit.out" "$OUT"
}
g1_done() {
    [ -n "$LOOP" ] && { kill "$LOOP" 2>/dev/null; wait "$LOOP" 2>/dev/null; LOOP=""; }
    pkill -f "$C/opt-theia/" 2>/dev/null; sleep 0.2
}

g1_setup healthy-commits "$W/app.tar.gz"; g1_app
check "G1.healthy-commits: install laid app.json into the release" test -f "$R/releases/demo-1.1/app.json"
g1_commit
check "G1.healthy-commits: commit PASSES (rc 0)" rc_is 0
check "G1.healthy-commits: ... names the process OK" said "app process 'demo_node' OK"
check "G1.healthy-commits: ... from the new release" said "runs $(readlink -f "$R")/releases/demo-1.1/bin/demo_fc"
g1_done

g1_setup missing-rolls-back "$W/app.tar.gz"
g1_commit
check "G1.missing-rolls-back: log names the process as MISSING" said "app process 'demo_node' MISSING"
g1_rolls_back missing-rolls-back
g1_done

g1_setup restart-loop-rolls-back "$W/app.tar.gz"
( while :; do "$R/current/bin/demo_fc" 1; done ) & LOOP=$!
g1_commit
check "G1.restart-loop-rolls-back: log names the process as RESTARTING" said "app process 'demo_node' RESTARTING"
g1_rolls_back restart-loop-rolls-back
g1_done

g1_setup old-binary-rolls-back "$W/app.tar.gz"
"$R/releases/demo-1.0/bin/demo_fc" 300 & SUP_PIDS="$SUP_PIDS $!"
sleep 0.2; rm -f "$R/releases/demo-1.0/bin/demo_fc"     # the old file is gone under it
g1_commit
check "G1.old-binary-rolls-back: log names the process as OLD BINARY" said "app process 'demo_node' OLD BINARY"
check "G1.old-binary-rolls-back: ... and the deleted path" said "demo-1.0/bin/demo_fc (deleted)"
g1_rolls_back old-binary-rolls-back
g1_done

g1_setup hook-exit1-rolls-back "$W/app-hookfail.tar.gz"; g1_app
g1_commit
check "G1.hook-exit1-rolls-back: processes passed first" said "app process 'demo_node' OK"
check "G1.hook-exit1-rolls-back: log says hook exit 1" said "health hook exit 1"
check "G1.hook-exit1-rolls-back: ... with the hook's last stdout line" said "health hook said: no MavLink heartbeat"
g1_rolls_back hook-exit1-rolls-back
g1_done

g1_setup hook-timeout-rolls-back "$W/app-hookslow.tar.gz"; g1_app
g1_commit
check "G1.hook-timeout-rolls-back: log says TIMED OUT" said "health hook TIMED OUT after 2s"
g1_rolls_back hook-timeout-rolls-back
g1_done

g1_setup hook-sees-swp-lib "$W/app-hookok.tar.gz"; g1_app
g1_commit
check "G1.hook-sees-swp-lib: commit PASSES with the hook (rc 0)" rc_is 0
check "G1.hook-sees-swp-lib: hook ran with LD_LIBRARY_PATH = the release lib, cwd = the release" \
      said "health hook said: lib ok: LD_LIBRARY_PATH=$(readlink -f "$R")/releases/demo-1.1/lib cwd=$(readlink -f "$R")/releases/demo-1.1"
check "G1.hook-sees-swp-lib: only the LAST stdout line is logged" sh -c "! grep -q 'said: starting' '$OUT'"
g1_done

g1_setup no-processes-supervisor-only "$W/app-noprocs.tar.gz"
g1_commit
check "G1.no-processes-supervisor-only: commit PASSES on the supervisor alone" rc_is 0
check "G1.no-processes-supervisor-only: ... with a WARNING" said "WARNING — the SWP declares no processes"
g1_done

g1_setup swp-timing-honoured "$W/app-timing.tar.gz"
g1_commit -u THEIA_OTA_HEALTH_WAIT_S -u THEIA_OTA_APP_STABLE_S -u THEIA_OTA_HOOK_TIMEOUT_S THEIA_OTA_HEALTH_STABLE=2
check "G1.swp-timing-honoured: app.json gate timings used" said "commit gate: wait 4s, app stable 1s, hook timeout 3s"
check "G1.swp-timing-honoured: ... the failure reports the SWP's 4 s budget" said "MISSING .*(gate 4s)"
g1_rolls_back swp-timing-honoured
g1_done
g1_setup swp-timing-env-overrides "$W/app-timing.tar.gz"
# shellcheck disable=SC2086
g1_commit $G1T THEIA_OTA_HEALTH_WAIT_S=3
check "G1.swp-timing-env-overrides: env THEIA_OTA_HEALTH_WAIT_S beats app.json" said "commit gate: wait 3s, app stable 2s, hook timeout 2s"
g1_done

# clamping (a real 300 s gate is too slow for a test: drive the timing helper)
printf '%s\n' '{"gate": {"wait_s": 900, "stable_s": 120, "hook_timeout_s": 600}, "processes": []}' > "$TMP/clamp.json"
timing() {   # $1 = env value, $2 = key, $3 = default, $4 = cap
    THEIA_SWP_SOURCE_ONLY=1 sh -c '. "$1"; _gate_timing "$2" "$3" "$4" "$5" "$6"' _ \
        "$SWP" "$1" "$TMP/clamp.json" "$2" "$3" "$4" 2>"$OUT"
}
check "G1.swp-timing-clamped: wait_s 900 → 300" test "$(timing '' wait_s 60 300)" = 300
check "G1.swp-timing-clamped: ... with a log line" said "gate.wait_s=900 from app.json CLAMPED to the rig cap 300"
check "G1.swp-timing-clamped: stable_s 120 → 60" test "$(timing '' stable_s 10 60)" = 60
check "G1.swp-timing-clamped: hook_timeout_s 600 → 60" test "$(timing '' hook_timeout_s 10 60)" = 60
check "G1.swp-timing-clamped: env is not capped (lab override)" test "$(timing 900 wait_s 60 300)" = 900
check "G1.swp-timing-clamped: no gate block → default" \
      sh -c "printf '{}' > '$TMP/clamp.json'; test \"\$(THEIA_SWP_SOURCE_ONLY=1 sh -c '. \"\$1\"; _gate_timing \"\" \"\$2\" wait_s 60 300' _ '$SWP' '$TMP/clamp.json' 2>/dev/null)\" = 60"

C="$TMP/g1-badname"; R="$C/opt-theia"; mkroot "$R"
mktree "$C/t" "demo-1.1" "$W/app-badname.tar.gz"
run "$R" "$SWP" ArtifactInstall "$C/t"
check "G1.invalid-process-name-refused: app.json exe '../../bin/sh' is refused at install" refused
check "G1.invalid-process-name-refused: ... nothing flipped" cur_is "$R" "releases/runtime-1.0"

# ==============================================================================
# theia-release
# ==============================================================================
R="$TMP/rel1/opt-theia"; mkroot "$R"
mktree "$TMP/rel1/tree" "runtime-2.0" "$W/rel-good.tar.gz" release.tar.gz
run "$R" "$REL" ArtifactInstall "$TMP/rel1/tree"
check "release: good artifact installs (rc 0)" rc_is 0
check "release: current → releases/runtime-2.0" cur_is "$R" "releases/runtime-2.0"
check "release: payload extracted" test -f "$R/releases/runtime-2.0/bin/com"
run "$R" "$REL" ArtifactCommit "$TMP/rel1/tree"
check "release: commit ok" rc_is 0

echo marker > "$R/releases/runtime-2.0/LIVE"
run "$R" "$REL" ArtifactInstall "$TMP/rel1/tree"
check "release: version == current is refused" refused
check "release: ... live tree intact" test -f "$R/releases/runtime-2.0/LIVE"
run "$R" "$REL" ArtifactRollback "$TMP/rel1/tree"
check "release: rollback after that refusal keeps the running release" \
      cur_is "$R" "releases/runtime-2.0"

for bad in '../..' '..' 'a/b' '/etc'; do
    C="$TMP/relbad"; rm -rf "$C"; R="$C/opt-theia"; mkroot "$R"
    echo keep > "$C/canary"
    mktree "$C/tree" "$bad" "$W/rel-good.tar.gz" release.tar.gz
    run "$R" "$REL" ArtifactInstall "$C/tree"
    check "release: version.txt '$bad' is refused" refused
    check "release: ... '$bad': nothing outside releases/ touched" \
          sh -c "test -f '$C/canary' && test ! -e '$C/bin' && test ! -e '$R/bin/com' && test \"\$(ls '$R/releases')\" = runtime-1.0 && test \"\$(readlink '$R/current')\" = releases/runtime-1.0"
done

for kind in symlink hardlink absolute dotdot fifo; do
    C="$TMP/reltar-$kind"; R="$C/opt-theia"; mkroot "$R"
    mktree "$C/tree" "runtime-3.0" "$W/bad-$kind.tar.gz" release.tar.gz
    run "$R" "$REL" ArtifactInstall "$C/tree"
    check "release: archive with a $kind member is refused" refused
    check "release: ... $kind: current unchanged, nothing staged" \
          sh -c "test \"\$(readlink '$R/current')\" = releases/runtime-1.0 && test ! -e '$R/releases/runtime-3.0'"
done

R="$TMP/rel-noprev/opt-theia"; mkroot "$R"
run "$R" "$REL" ArtifactRollback ""
check "release: rollback WITHOUT previous fails" rc_fail
check "release: ... with a clear message" said "no previous release to roll back to"
R="$TMP/rel2/opt-theia"; mkroot "$R"
mktree "$TMP/rel2/tree" "runtime-2.0" "$W/rel-good.tar.gz" release.tar.gz
run "$R" "$REL" ArtifactInstall "$TMP/rel2/tree"
run "$R" "$REL" ArtifactRollback "$TMP/rel2/tree"
check "release: rollback with a previous succeeds" rc_is 0
check "release: ... current → releases/runtime-1.0" cur_is "$R" "releases/runtime-1.0"

# ==============================================================================
# theia-app (needs python3, like the module)
# ==============================================================================
if command -v python3 >/dev/null 2>&1; then
    mkapp() {   # $1 = dir, $2 = app name (as JSON string content)
        mkdir -p "$1/src/bin"
        echo appfc > "$1/src/bin/app_fc"
        printf '{"name": "%s", "version": "1.0", "executor_subtree": {"nodes": [{"name": "app_fc"}]}}\n' "$2" > "$1/src/app.json"
        ( cd "$1/src" && tar -czf "$1/app.tar.gz" . )
        mkdir -p "$1/tree/files"
        cp "$1/app.tar.gz" "$1/tree/files/app.tar.gz"
        cp "$1/src/app.json" "$1/tree/files/app.json"
        echo "$3" > "$1/tree/files/version.txt"
    }
    C="$TMP/app1"; R="$C/opt-theia"; mkroot "$R"
    mkapp "$C" "demo" "demo-1.0"
    run "$R" "$APP" ArtifactInstall "$C/tree"
    check "app: good artifact installs (rc 0)" rc_is 0
    check "app: FC overlaid into current/bin" test -f "$R/current/bin/app_fc"
    check "app: install record under apps/demo" test -f "$R/apps/demo/.artifact"
    check "app: executor merged" grep -q app_fc "$R/config/executor.json"

    for bad in '../../escape' '..' 'a/b' '/abs'; do
        C="$TMP/appbad"; rm -rf "$C"; R="$C/opt-theia"; mkroot "$R"
        echo keep > "$C/canary"
        mkapp "$C" "$bad" "demo-1.0"
        run "$R" "$APP" ArtifactInstall "$C/tree"
        check "app: app.json name '$bad' is refused" refused
        check "app: ... '$bad': no record dir, nothing outside apps/, no overlay" \
              sh -c "test -f '$C/canary' && test ! -e '$C/escape' && test ! -e '$R/escape' && test ! -e '$R/apps' && test ! -e '$R/current/bin/app_fc'"
    done
    C="$TMP/appbadver"; R="$C/opt-theia"; mkroot "$R"
    mkapp "$C" "demo" "../../x"
    run "$R" "$APP" ArtifactInstall "$C/tree"
    check "app: version.txt '../../x' is refused" refused

    for kind in symlink hardlink absolute dotdot fifo; do
        C="$TMP/apptar-$kind"; R="$C/opt-theia"; mkroot "$R"
        mkapp "$C" "demo" "demo-1.0"
        cp "$W/bad-$kind.tar.gz" "$C/tree/files/app.tar.gz"
        run "$R" "$APP" ArtifactInstall "$C/tree"
        check "app: archive with a $kind member is refused" refused
        check "app: ... $kind: nothing overlaid or recorded" \
              sh -c "test ! -e '$R/apps' && test ! -e '$R/current/bin/demo_fc' && test ! -e '$R/current/bin/link'"
    done
else
    echo "skip - theia-app cases (no python3)"
fi

# ==============================================================================
# state script: Mender → UCM hand-off is fail-closed
# ==============================================================================
hook() {   # $1 = root, $2 = THEIA_OTA_ALLOW_NO_UCM value ("" = unset)
    if [ -n "$2" ]; then
        THEIA_ROOT="$1" THEIA_OTA_ALLOW_NO_UCM="$2" sh "$HOOK" >"$OUT" 2>&1; RC=$?
    else
        ( unset THEIA_OTA_ALLOW_NO_UCM; THEIA_ROOT="$1" sh "$HOOK" ) >"$OUT" 2>&1; RC=$?
    fi
    [ "$VERBOSE" = 1 ] && sed 's/^/       > /' "$OUT"
    return 0
}
stub() {   # $1 = root, $2 = exit code of the theia-migrate stub
    # shellcheck disable=SC2016  # $@ / ${…} belong to the generated stub
    printf '#!/bin/sh\necho "$@" > "%s/argv"\necho "strict=${THEIA_MIGRATE_ADOPT_STRICT:-}" > "%s/env"\nexit %s\n' \
        "$1" "$1" "$2" > "$1/bin/theia-migrate"
    chmod 755 "$1/bin/theia-migrate"
}
R="$TMP/hook/opt-theia"; mkroot "$R"
hook "$R" ""
check "hook: no adopter → FAILS by default" rc_fail
hook "$R" 1
check "hook: no adopter + THEIA_OTA_ALLOW_NO_UCM=1 → ok" rc_is 0
check "hook: ... as a symlink-only install" said "symlink-only install"
stub "$R" 0; hook "$R" ""
check "hook: UCM accepts (adopter exit 0) → ok" rc_is 0
check "hook: adopter called as 'adopt <ver> full'" \
      test "$(cat "$R/argv")" = "adopt runtime-1.0 full"
check "hook: adopter asked for strict exit codes" test "$(cat "$R/env")" = "strict=1"
stub "$R" 3; hook "$R" ""
check "hook: UCM unreachable (adopter exit 3) → FAILS by default" rc_fail
check "hook: ... with a clear message" said "UCM unreachable"
hook "$R" 1
check "hook: UCM unreachable + THEIA_OTA_ALLOW_NO_UCM=1 → ok" rc_is 0
stub "$R" 1; hook "$R" ""
check "hook: UCM reject (adopter exit 1) → fails" rc_fail
hook "$R" 1
check "hook: UCM reject fails EVEN with THEIA_OTA_ALLOW_NO_UCM=1" rc_fail
R="$TMP/hook-nocur/opt-theia"; mkdir -p "$R"
hook "$R" 1
check "hook: no current release → fails" rc_fail

echo
echo "test_modules.sh: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]

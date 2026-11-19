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

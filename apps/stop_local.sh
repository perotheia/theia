#!/usr/bin/env bash
# demo/stop_local.sh — tear down a local single-machine run started by
# stage_local.sh + `./supervisor`.
#
# `killall supervisor` is NOT enough: when the supervisor dies (especially
# with -9) it cannot reap its children, so the bin/<node> processes reparent
# to init (PPID 1) and keep running — often pinning CPU. This script kills the
# whole theia process family: the supervisor, every supervised node binary, the
# standalone services-log collector (if any), and stale `tdb logcat` clients.
#
# Usage:
#   demo/stop_local.sh           # graceful (SIGTERM), then SIGKILL stragglers
#   demo/stop_local.sh -9        # go straight to SIGKILL
#   demo/stop_local.sh -n        # dry run — list what WOULD be killed
set -uo pipefail

SIG_FIRST="TERM"
DRY=0
for a in "$@"; do
    case "$a" in
        -9|--kill)  SIG_FIRST="KILL" ;;
        -n|--dry)   DRY=1 ;;
        -h|--help)
            grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown arg: $a (try -h)" >&2; exit 2 ;;
    esac
done

# Patterns that identify a theia process. The supervisor first (so it stops
# trying to restart children), then the node binaries + collector + clients.
#   - '\./supervisor'         the engine (argv[0] is ./supervisor)
#   - '\./bin/<node>'         each supervised worker binary (bin/sm, bin/p1, …)
#   - 'services-log'          the standalone trace collector, if staged that way
#   - 'tdb logcat'            a follower client holding a hub subscription
PATTERNS=(
    '[.]/supervisor'
    '[.]/bin/(sm|log|per|ucm|shwa|p1|p2|p3)'
    'services-log'
    'tdb logcat'
)

# Collect matching PIDs (excluding this script + its own grep/pgrep).
collect() {
    local self=$$
    for pat in "${PATTERNS[@]}"; do
        pgrep -f "$pat"
    done | sort -u | grep -vx "$self"
}

pids=$(collect)
if [[ -z "$pids" ]]; then
    echo "nothing to stop — no theia processes running."
    exit 0
fi

echo "theia processes found:"
# shellcheck disable=SC2086
ps -o pid,ppid,etime,pcpu,cmd -p $pids 2>/dev/null | sed 's/^/  /'

if [[ "$DRY" == 1 ]]; then
    echo "(dry run — nothing killed)"
    exit 0
fi

# First pass: the chosen signal (TERM by default → supervisor shuts its tree
# down in order; KILL if asked).
echo "sending SIG$SIG_FIRST ..."
# shellcheck disable=SC2086
kill -"$SIG_FIRST" $pids 2>/dev/null

# Give graceful shutdown a moment, then SIGKILL anything still standing
# (orphaned nodes that ignored TERM, or were already reparented to init).
if [[ "$SIG_FIRST" != "KILL" ]]; then
    sleep 2
    left=$(collect)
    if [[ -n "$left" ]]; then
        echo "SIGKILL stragglers: $left"
        # shellcheck disable=SC2086
        kill -9 $left 2>/dev/null
        sleep 1
    fi
fi

# Verify.
remaining=$(collect)
if [[ -z "$remaining" ]]; then
    echo "clean — all theia processes stopped."
else
    echo "WARNING: still alive after SIGKILL:" >&2
    # shellcheck disable=SC2086
    ps -o pid,ppid,cmd -p $remaining 2>/dev/null | sed 's/^/  /' >&2
    exit 1
fi

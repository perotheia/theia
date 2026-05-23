#!/usr/bin/env bash
# deploy/run-supervisor.sh — container entrypoint.
#
# Lifecycle:
#   1. Apply Puppet manifest at /etc/puppet/manifests/<host>.pp.
#      Puppet's `theia` module installs the .ipk (if present),
#      drops executor.yaml + machines.yaml, writes the systemd
#      unit, and `notify`s the supervisor service.
#   2. Detect whether supervisor is now installed. If not, fail
#      fast — there's no useful work to do.
#   3. Foreground-exec the supervisor binary with the executor.yaml
#      so it inherits Docker's signal handling (Ctrl-C / docker stop
#      → SIGTERM → graceful shutdown).
#
# Env vars:
#   HOSTNAME    — used to pick /etc/puppet/manifests/$HOSTNAME.pp
#                 (also forwarded to the supervisor as --hostname,
#                 if that flag exists; it currently doesn't but the
#                 wiring is here).
#   PUPPET_FLAGS — extra args to `puppet apply` (default: --detailed-exitcodes).

set -euo pipefail

readonly LOG_PREFIX="[run-supervisor]"

log() { echo "$LOG_PREFIX $*" >&2; }

# -----------------------------------------------------------------------------
# 1. Apply Puppet manifest.
# -----------------------------------------------------------------------------

HOSTNAME="${HOSTNAME:-$(hostname)}"
MANIFEST="/etc/puppet/manifests/${HOSTNAME}.pp"

if [[ ! -f "$MANIFEST" ]]; then
    log "ERROR: no Puppet manifest at $MANIFEST"
    log "       expected one of: $(ls /etc/puppet/manifests/ 2>/dev/null | tr '\n' ' ')"
    exit 2
fi

log "applying Puppet manifest $MANIFEST"

# `--detailed-exitcodes` returns:
#   0 — no changes, no errors
#   2 — changes applied, no errors
#   4 — errors (no changes attempted)
#   6 — errors AND changes attempted
# We treat 0 and 2 as success.
PUPPET_FLAGS="${PUPPET_FLAGS:---detailed-exitcodes --verbose}"

# Puppet wants writable directories for its `codedir`, `vardir`,
# and `confdir` working state. The bind-mounted /etc/puppet is
# read-only (we don't want a Puppet apply to scribble back into
# the workspace). Point Puppet's writable dirs into /var/lib/puppet
# inside the container.
mkdir -p /var/lib/puppet/code /var/lib/puppet/var /var/lib/puppet/conf

set +e
puppet apply $PUPPET_FLAGS \
    --confdir=/var/lib/puppet/conf \
    --codedir=/var/lib/puppet/code \
    --vardir=/var/lib/puppet/var \
    --modulepath=/etc/puppet/modules \
    "$MANIFEST"
rc=$?
set -e

case "$rc" in
    0) log "Puppet: no changes" ;;
    2) log "Puppet: changes applied successfully" ;;
    4|6) log "ERROR: Puppet apply failed (rc=$rc)"; exit "$rc" ;;
    *) log "ERROR: unexpected Puppet exit code $rc"; exit "$rc" ;;
esac

# -----------------------------------------------------------------------------
# 2. Verify supervisor is installed.
# -----------------------------------------------------------------------------

SUPERVISOR_BIN="/usr/bin/theia-supervisor"
EXECUTOR_YAML="/etc/theia/executor.yaml"

if [[ ! -x "$SUPERVISOR_BIN" ]]; then
    log "ERROR: supervisor binary not found at $SUPERVISOR_BIN after Puppet apply"
    log "       Puppet should have installed it via opkg or bind-mount."
    log "       Check the Puppet manifest's theia::install / opkg::package"
    log "       resource — it may not have run, or the .ipk may be missing."
    exit 3
fi

if [[ ! -f "$EXECUTOR_YAML" ]]; then
    log "ERROR: no executor.yaml at $EXECUTOR_YAML after Puppet apply"
    log "       Puppet should have dropped one for this host. Check the"
    log "       theia::config resource in the manifest."
    exit 4
fi

# -----------------------------------------------------------------------------
# 3. Exec the supervisor as PID 1 (or close to it — `exec` swaps the
#    current process, so signals from `docker stop` reach the
#    supervisor directly).
# -----------------------------------------------------------------------------

log "supervisor binary ready: $SUPERVISOR_BIN"
log "executor.yaml ready:    $EXECUTOR_YAML"
log "starting supervisor (foreground)"

exec "$SUPERVISOR_BIN" run "$EXECUTOR_YAML" --root-dir /opt/theia

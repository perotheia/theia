#!/usr/bin/env bash
# deploy/run-supervisor.sh — container entrypoint.
#
# This is NOT a provisioner. The container does no self-provisioning: it just
# runs whatever has already been installed on disk. Provisioning + orchestration
# are driven FROM OUTSIDE (puppet pushed in by an operator / CI), which lays down
#   /opt/theia/bin/supervisor          — the supervisor binary (per-machine .ipk)
#   /opt/theia/config/executor.json    — the supervision tree for this machine
# On the NEXT container start the entrypoint picks them up and runs.
#
# Until both exist, the entrypoint LOOPS with a "waiting for orchestration"
# message rather than exiting — so the container stays alive for an operator to
# provision into, and a reboot after provisioning Just Works.
#
# `exec` the supervisor so Docker's signals (docker stop → SIGTERM) reach it
# directly for graceful shutdown.

set -euo pipefail

log() { echo "[run-supervisor] $*" >&2; }

SUPERVISOR_BIN="/opt/theia/bin/supervisor"
EXECUTOR_JSON="/opt/theia/config/executor.json"

# Wait until provisioning has put both the binary and the config in place.
while [[ ! -x "$SUPERVISOR_BIN" || ! -f "$EXECUTOR_JSON" ]]; do
    log "waiting for orchestration: need $SUPERVISOR_BIN (exec) + $EXECUTOR_JSON"
    sleep 10
done

log "supervisor ready — starting (foreground)"
log "  binary:   $SUPERVISOR_BIN"
log "  manifest: $EXECUTOR_JSON"

# The supervisor (gen-app FC) takes NO argv — it reads its manifest + child
# working-dir root from the environment.
export THEIA_SUPERVISOR_MANIFEST="$EXECUTOR_JSON"
export THEIA_ROOT_DIR="/opt/theia"
exec "$SUPERVISOR_BIN"

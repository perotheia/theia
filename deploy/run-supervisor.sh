#!/usr/bin/env bash
# deploy/run-supervisor.sh — container entrypoint + puppet convergence driver.
#
# Puppet is the always-on lifecycle agent. On a real host systemd runs it; in
# docker this entrypoint runs it on each (re)start:
#
#   1. puppet apply theia::provisioning   — etcd (one per cluster) + EMPTY
#                                            executor.json (supervisor can boot).
#   2. puppet apply theia::orchestration  — install the per-machine .ipk
#                                            (supervisor + FCs) + the REAL
#                                            executor.json + setcap.
#   3. exec /opt/theia/bin/supervisor with the (real) executor.json.
#
# If the supervisor exits abnormally, we FALL BACK to puppet (re-converge:
# re-run orchestration, then re-exec) rather than just dying — a transient
# config/binary issue self-heals on the next pass.
#
# $machine = $HOSTNAME (compose sets central / compute). Manifests are
# machine-generic: provisioning/orchestration read <machine>/{machine,
# application,execution,executor}.json. No per-host .pp.

set -uo pipefail

log() { echo "[run-supervisor] $*" >&2; }

MACHINE="${HOSTNAME:-$(hostname)}"
PUPPET_MODULES="/etc/puppet/modules"
PUPPET_SITE="/etc/puppet"
HIERA="/etc/puppet/hiera.yaml"
SUPERVISOR_BIN="/opt/theia/bin/supervisor"
EXECUTOR_JSON="/opt/theia/config/executor.json"

# Puppet needs writable working dirs (the bind-mounted /etc/puppet is read-only).
mkdir -p /var/lib/puppet/code /var/lib/puppet/var /var/lib/puppet/conf

# $machine reaches the machine-generic classes via FACTER_theia_machine.
export FACTER_theia_machine="$MACHINE"

# Bundled shared libs (e.g. libetcd-cpp-api.so for per) land at /opt/theia/lib
# from the .ipk; the supervisor + its execvp'd children inherit this so they
# resolve them at runtime.
export LD_LIBRARY_PATH="/opt/theia/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# This machine's supervisor TIPC instance, from the manifest (ARA Executor
# identity — central=0, compute=1, so two supervisors on one host TIPC namespace
# don't collide). Read from <machine>/execution.json.supervisor_instance; the
# supervisor binary picks it up via THEIA_SUPERVISOR_INSTANCE. Default 0.
_exec_json="/etc/theia/manifest/${MACHINE}/execution.json"
if [[ -f "$_exec_json" ]]; then
    _inst="$(grep -o '"supervisor_instance"[[:space:]]*:[[:space:]]*[0-9]*' "$_exec_json" \
             | sed 's/.*:[[:space:]]*//')"
    export THEIA_SUPERVISOR_INSTANCE="${_inst:-0}"
    log "supervisor TIPC instance=${THEIA_SUPERVISOR_INSTANCE} (from manifest)"
fi

puppet_apply() {  # $1 = site manifest (provisioning.pp / orchestration.pp)
    log "puppet apply $1 (machine=$MACHINE)"
    puppet apply \
        --confdir=/var/lib/puppet/conf \
        --codedir=/var/lib/puppet/code \
        --vardir=/var/lib/puppet/var \
        --modulepath="$PUPPET_MODULES" \
        --hiera_config="$HIERA" \
        "$PUPPET_SITE/$1"
}

converge() {
    puppet_apply provisioning.pp  || log "WARN: provisioning had errors"
    puppet_apply orchestration.pp || log "WARN: orchestration had errors"
}

# 1+2. Provision then orchestrate.
converge

# 3. Run the supervisor; on abnormal exit, re-converge and retry (bounded).
attempt=0
while true; do
    if [[ -x "$SUPERVISOR_BIN" && -f "$EXECUTOR_JSON" ]]; then
        log "starting supervisor (machine=$MACHINE, manifest=$EXECUTOR_JSON)"
        export THEIA_SUPERVISOR_MANIFEST="$EXECUTOR_JSON"
        export THEIA_ROOT_DIR="/opt/theia"
        "$SUPERVISOR_BIN"
        rc=$?
        log "supervisor exited rc=$rc"
        [[ $rc -eq 0 ]] && exit 0   # clean shutdown (docker stop)
    else
        log "supervisor binary or executor.json missing after converge"
    fi
    attempt=$((attempt + 1))
    if (( attempt > 5 )); then
        log "FATAL: supervisor failed to stay up after $attempt converge attempts"
        exit 1
    fi
    log "re-converging (attempt $attempt) — puppet fallback"
    sleep 3
    converge
done

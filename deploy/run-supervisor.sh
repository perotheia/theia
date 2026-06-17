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

    # This machine's logger SINK policy (un-expanded THEIA_LOGGER, e.g.
    # "file:/var/log/theia" or "syslog"). The supervisor serves it back to
    # log[logging] via GetLoggerPolicy so the hose knows what to tail. Default
    # file:/tmp/theia matches build_supervisor_tree's fallback.
    _logpol="$(grep -o '"logger_policy"[[:space:]]*:[[:space:]]*"[^"]*"' "$_exec_json" \
               | sed 's/.*:[[:space:]]*"\(.*\)"/\1/')"
    export THEIA_LOGGER_POLICY="${_logpol:-file:/tmp/theia}"
    log "logger policy=${THEIA_LOGGER_POLICY} (from manifest)"
fi

# This machine's CLUSTER INDEX (central=0, compute=1) — the TIPC INSTANCE every
# node on this machine binds: a service has a stable TIPC type and a per-machine
# instance, so central's supervisor:0/shwa:0 and compute's supervisor:1/shwa:1
# coexist on the SHARED host TIPC namespace (network_mode: host). Read from
# <machine>/machine.json (Machine.machine_index). The supervisor reads
# THEIA_MACHINE_INSTANCE as its boot knob to shift each CHILD's --tipc instance;
# its OWN ctl address is the --tipc passed to the exec below. Default 0.
_machine_json="/etc/theia/manifest/${MACHINE}/machine.json"
THEIA_MACHINE_INSTANCE=0
if [[ -f "$_machine_json" ]]; then
    _midx="$(grep -o '"machine_index"[[:space:]]*:[[:space:]]*[0-9]*' "$_machine_json" \
             | sed 's/.*:[[:space:]]*//')"
    THEIA_MACHINE_INSTANCE="${_midx:-0}"
fi
export THEIA_MACHINE_INSTANCE
log "machine instance=${THEIA_MACHINE_INSTANCE} (from machine.json)"

# The CLUSTER manifest root — machines.json (the index→name list) + each
# <machine>/machine.json. com inherits this from the supervisor's env and reads
# it to map a TIPC instance (e.g. an AccelSample's machine_index) back to a
# machine NAME (central/compute) for the GUI. The supervisor itself doesn't use
# it; it just passes it down to every child it forks.
if [[ -d "/etc/theia/manifest" ]]; then
    export THEIA_MACHINE_MANIFEST="/etc/theia/manifest"
    log "machine manifest root=${THEIA_MACHINE_MANIFEST} (com inherits)"
fi

# This machine's mTLS material (staged by `theia manifest` into
# <machine>/certs/, bind-mounted read-only here). When present, com serves
# MUTUAL TLS (requires + verifies the client cert against the CA) on all three
# gRPC ports, and the supervisor passes these down to com via the inherited env.
# Absent → com runs INSECURE (the no-cert dev default). rtdb/GUI use the SAME
# ca.crt + client.{crt,key} to connect.
_certs_dir="/etc/theia/manifest/${MACHINE}/certs"
if [[ -f "${_certs_dir}/server.crt" && -f "${_certs_dir}/server.key" ]]; then
    export THEIA_COM_TLS_CERT="${_certs_dir}/server.crt"
    export THEIA_COM_TLS_KEY="${_certs_dir}/server.key"
    [[ -f "${_certs_dir}/ca.crt" ]] && export THEIA_COM_TLS_CA="${_certs_dir}/ca.crt"
    log "mTLS on — com serves TLS (cert=${THEIA_COM_TLS_CERT}, CA=${THEIA_COM_TLS_CA:-none})"
else
    log "no certs at ${_certs_dir} — com runs INSECURE"
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
        log "starting supervisor (machine=$MACHINE, manifest=$EXECUTOR_JSON, instance=$THEIA_MACHINE_INSTANCE)"
        export THEIA_SUPERVISOR_MANIFEST="$EXECUTOR_JSON"
        export THEIA_ROOT_DIR="/opt/theia"
        # The supervisor's own SupervisorCtl binds at this machine's instance via
        # --tipc (instance-only ":N" keeps the compiled type 0x80020001). central
        # (:0) omits it (compiled default); compute (:1) passes it.
        _sup_args=()
        [[ "$THEIA_MACHINE_INSTANCE" != "0" ]] && \
            _sup_args+=("--tipc=supervisor_ctl=:${THEIA_MACHINE_INSTANCE}")
        "$SUPERVISOR_BIN" "${_sup_args[@]}"
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

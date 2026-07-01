#!/usr/bin/env bash
# deploy/run-supervisor.sh — container entrypoint + Mender-style bring-up driver.
#
# PUPPET IS GONE. The on-device UCM agent (services/ucm) is the always-on update
# lifecycle agent (release-dir install/switch/rollback + config). This entrypoint
# owns only FIRST-BOOT bring-up + crash re-exec:
#
#   1. THIN provisioning (the 3 independent steps run IN PARALLEL — Mender-style,
#      replacing the old serial puppet provision→orchestrate):
#        (a) stage the current release tree + current symlink,
#        (b) executor.json + per-FC config → /opt/theia/config/,
#        (c) setcap the cap-needing binaries.
#   2. exec /opt/theia/bin/supervisor (forks the FCs INCLUDING the UCM agent).
#   3. first-boot config seed (per all-FC defaults → etcd) once per/etcd are up.
#   4. on abnormal supervisor exit, re-provision + re-exec (bounded) — a transient
#      config/binary issue self-heals; deliberate updates are the UCM agent's job.
#
# $machine = $HOSTNAME (compose sets central / compute). Inputs are
# machine-generic: read <machine>/{machine,execution,executor}.json + config/.

set -uo pipefail

log() { echo "[run-supervisor] $*" >&2; }

MACHINE="${HOSTNAME:-$(hostname)}"
SUPERVISOR_BIN="/opt/theia/bin/supervisor"
EXECUTOR_JSON="/opt/theia/config/executor.json"

# $machine reaches machine-generic reads via this fact (kept for compatibility).
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

# This rig's TIPC CLUSTERID (network id) — multi-rig test isolation. TIPC nodes
# only peer when their clusterid matches, so a rig with a DISTINCT clusterid is in
# its own isolated TIPC network on a shared switch (no (type,instance) collisions
# with other rigs). Read Machine.tipc_cluster_id (default 4711 = the TIPC default,
# shared — unchanged). Set BEFORE any TIPC traffic (the supervisor is the first
# TIPC user), and only when non-default + the tooling/caps are present. Best-
# effort: a no-CAP_NET_ADMIN host logs a warning + runs on the default.
_clusterid=4711
if [[ -f "$_machine_json" ]]; then
    _cid="$(grep -o '"tipc_cluster_id"[[:space:]]*:[[:space:]]*[0-9]*' "$_machine_json" \
            | sed 's/.*:[[:space:]]*//')"
    _clusterid="${_cid:-4711}"
fi
if [[ "$_clusterid" != "4711" ]]; then
    modprobe tipc 2>/dev/null || true
    if command -v tipc >/dev/null 2>&1 \
       && tipc node set clusterid "$_clusterid" 2>/dev/null; then
        log "TIPC clusterid=${_clusterid} (rig isolation; tipc node set)"
    else
        log "WARN: could not set TIPC clusterid=${_clusterid} (tipc tool / "\
"CAP_NET_ADMIN missing?) — running on the default 4711"
    fi
fi
export THEIA_TIPC_CLUSTERID="${_clusterid}"

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

# ── Mender-style THIN provisioning (replaces the Puppet converge) ───────────
#
# Puppet is gone. The on-device UCM agent (services/ucm) owns deliberate updates
# (release-dir install/switch/rollback + config). This entrypoint owns only
# first-boot BRING-UP + crash re-exec. The prep splits into INDEPENDENT steps run
# in PARALLEL (provision ∥, not the old serial provision→orchestrate):
#   (a) stage the current release tree:  /opt/theia/releases/<ver> + current→it,
#       /opt/theia/bin → current/bin (so a UCM switch re-aims the platform).
#   (b) stage executor.json + per-FC config into /opt/theia/config/.
#   (c) setcap the cap-needing binaries (fw/nm/idsm/supervisor).
# etcd is provided by the compose `etcd` service (FACTER_theia_etcd_external).
THEIA_VERSION="${THEIA_VERSION:-1.0.0}"
RELEASES="/opt/theia/releases"
CURRENT="/opt/theia/current"

provision_release() {            # (a) release-dir tree + current symlink
    local rel="$RELEASES/$THEIA_VERSION"
    mkdir -p "$rel"/{bin,lib,config,migrations,hooks}
    # The .ipk / compose mount lands binaries at /opt/theia/bin + libs at /lib.
    # Mirror them into the release (cp -al = hardlink, cheap) so `current` is the
    # source of truth the UCM agent switches.
    [[ -d /opt/theia/bin ]] && cp -aln /opt/theia/bin/.  "$rel/bin/"  2>/dev/null || true
    [[ -d /opt/theia/lib ]] && cp -aln /opt/theia/lib/.  "$rel/lib/"  2>/dev/null || true
    # Atomically point current → this release (rename of a temp symlink).
    ln -sfn "$rel" "$CURRENT.tmp" && mv -Tf "$CURRENT.tmp" "$CURRENT"
    log "provision(a): release $THEIA_VERSION staged, current → $rel"
}

provision_config() {             # (b) executor.json + per-FC config
    local src="/etc/theia/manifest/${MACHINE}"
    mkdir -p /opt/theia/config
    [[ -f "$src/executor.json" ]] && cp -f "$src/executor.json" /opt/theia/config/
    if [[ -d "$src/config" ]]; then
        cp -f "$src"/config/*.json /opt/theia/config/ 2>/dev/null || true
        log "provision(b): executor.json + $(ls "$src"/config/*.json 2>/dev/null | wc -l) FC config(s) staged"
    else
        log "provision(b): executor.json staged (no per-FC config dir)"
    fi
}

provision_caps() {               # (c) setcap the cap-needing binaries
    command -v setcap >/dev/null || { log "provision(c): no setcap — skip"; return 0; }
    for b in supervisor fw nm idsm; do
        [[ -x "/opt/theia/bin/$b" ]] && \
            setcap cap_net_admin,cap_net_raw,cap_sys_nice+ep "/opt/theia/bin/$b" 2>/dev/null || true
    done
    log "provision(c): setcap applied (best-effort)"
}

converge() {
    # Run the three independent provisioning steps IN PARALLEL, wait for all.
    provision_release & provision_config & provision_caps &
    wait
    log "thin provisioning complete (parallel) — no puppet"
}

# 1. Provision (parallel, Mender-style — replaces puppet provision→orchestrate).
converge

# 2. First-boot config seed (background): once per/etcd are up, push EVERY FC's
#    declared config defaults into etcd (the PREP-B all-FC seeder). Only the
#    etcd-host machine seeds (central); idempotent + best-effort + bounded retry
#    (per is forked by the supervisor a few seconds in). NOT on the critical
#    path — the supervisor boots regardless.
seed_config() {
    local tool="/opt/theia/migration/seed.py"
    local defs="/opt/theia/config/seed_defaults.json"
    local schema="/opt/theia/config/seed_schema.json"
    [[ -f "$tool" && -f "$defs" ]] || { log "seed: no seeder/defaults — skip"; return 0; }
    export THEIA_ROOT="/opt/theia"
    for _ in $(seq 1 7); do
        sleep 3
        if python3 "$tool" defaults --defaults "$defs" --schema "$schema" \
               >/dev/null 2>&1; then
            log "seed: all-FC config defaults seeded into etcd (idempotent)"
            return 0
        fi
    done
    log "seed: defaults did not land within ~20s (per slow?) — non-fatal"
}
# Only the etcd host seeds (one writer per cluster). central declares etcd.
if [[ "${FACTER_theia_etcd_external:-0}" == "1" || -f "/opt/theia/config/seed_defaults.json" ]]; then
    seed_config &
fi

# 3. Run the supervisor; on abnormal exit, re-converge and retry (bounded).
attempt=0
while true; do
    if [[ -x "$SUPERVISOR_BIN" && -f "$EXECUTOR_JSON" ]]; then
        log "starting supervisor (machine=$MACHINE, manifest=$EXECUTOR_JSON, instance=$THEIA_MACHINE_INSTANCE)"
        export THEIA_SUPERVISOR_MANIFEST="$EXECUTOR_JSON"
        export THEIA_ROOT_DIR="/opt/theia"
        export THEIA_INSTALL_DIR="/opt/theia/current"
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
    log "re-provisioning (attempt $attempt) — thin re-converge + re-exec"
    sleep 3
    converge
done

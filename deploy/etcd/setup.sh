#!/usr/bin/env bash
# deploy/etcd/setup.sh — install + start the Theia etcd daemon.
#
# Reproduces what was done manually in phase 1 of the etcd-state-backbone
# plan. Idempotent: re-running is safe and a no-op if etcd is already
# up.
#
# Architecture:
#   - apt-installed etcd 3.3 (jammy ships this); v3 API used everywhere
#   - single-node localhost; not clustered, not encrypted, not auth'd
#   - data dir at /var/lib/theia-etcd (separate from apt's
#     /var/lib/etcd so the apt-shipped etcd.service can coexist
#     disabled)
#   - listens on 127.0.0.1:2379 AND 172.30.0.1:2379 (the docker compose
#     theia_net bridge gateway) so compose containers can reach it
#
# Verify after:
#   ETCDCTL_API=3 etcdctl --endpoints=127.0.0.1:2379 endpoint status -w table

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNIT_NAME="theia-etcd.service"
UNIT_SRC="$SCRIPT_DIR/$UNIT_NAME"
UNIT_DST="/etc/systemd/system/$UNIT_NAME"
DATA_DIR="/var/lib/theia-etcd"

step() { echo "==> $*"; }

# --- preconditions --------------------------------------------------

if ! command -v etcd >/dev/null; then
    step "installing etcd-server etcd-client (apt)"
    sudo apt-get update -qq
    sudo apt-get install -y --no-install-recommends etcd-server etcd-client
fi

# apt-shipped etcd.service binds the same ports; turn it off so our
# theia-etcd.service can claim them.
if systemctl is-enabled --quiet etcd 2>/dev/null; then
    step "disabling apt's etcd.service (we run our own unit)"
    sudo systemctl stop etcd 2>/dev/null || true
    sudo systemctl disable etcd 2>/dev/null || true
fi

# --- data dir ------------------------------------------------------

if [[ ! -d "$DATA_DIR" ]]; then
    step "creating $DATA_DIR (owned by etcd:etcd)"
    sudo mkdir -p "$DATA_DIR"
    sudo chown etcd:etcd "$DATA_DIR"
fi

# --- systemd unit --------------------------------------------------

if ! cmp -s "$UNIT_SRC" "$UNIT_DST" 2>/dev/null; then
    step "installing $UNIT_NAME"
    sudo cp "$UNIT_SRC" "$UNIT_DST"
    sudo systemctl daemon-reload
fi

if ! systemctl is-enabled --quiet "$UNIT_NAME"; then
    step "enabling $UNIT_NAME"
    sudo systemctl enable "$UNIT_NAME"
fi

if ! systemctl is-active --quiet "$UNIT_NAME"; then
    step "starting $UNIT_NAME"
    sudo systemctl start "$UNIT_NAME"
    sleep 2
fi

# --- smoke ---------------------------------------------------------

step "smoke: host can put + get"
ETCDCTL_API=3 etcdctl --endpoints=127.0.0.1:2379 \
    put /theia/setup-smoke "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >/dev/null
val=$(ETCDCTL_API=3 etcdctl --endpoints=127.0.0.1:2379 \
    get /theia/setup-smoke --print-value-only)
if [[ -z "$val" ]]; then
    echo "smoke failed: get returned empty" >&2
    exit 1
fi
ETCDCTL_API=3 etcdctl --endpoints=127.0.0.1:2379 \
    del /theia/setup-smoke >/dev/null

echo
echo "theia-etcd ready."
echo "  host       : etcdctl --endpoints=127.0.0.1:2379"
echo "  containers : etcdctl --endpoints=172.30.0.1:2379"
echo "  v3 HTTP    : curl -X POST http://127.0.0.1:2379/v3beta/kv/range -d '{\"key\":\"L3RoZWlh\",\"range_end\":\"L3RoZWli\"}'"
echo "             (keys/values base64-encoded; /theia → L3RoZWlh, /theib → L3RoZWli)"

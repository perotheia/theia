#!/usr/bin/env bash
# deploy/mender/server/enroll-rig.sh — enrol a rig's mender client to the local
# Mender server (deploy/mender/server/up.sh), so it appears in the web UI and can
# pull theia-release OTA. Run from the controller (the dev box); reaches the rig
# over SSH and the server over its API.
#
# What it does (the steps proven on rig1-central → dalek server):
#   1. ship the server's self-signed CA to the rig + trust it
#   2. add the cert vhost (docker.mender.io) → server IP in the rig's /etc/hosts
#   3. write /etc/mender/mender.conf (ServerURL + the CA)
#   4. install minimal identity + inventory scripts (Debian's mender-client ships
#      none) — identity = NIC MAC, inventory = device_type/hostname/IP
#   5. bootstrap + (re)start the client → it submits an auth request (lands pending)
#   6. accept the device via the server's device-auth API → accepted
#   7. restart the client → it authorizes + submits inventory (shows in the UI)
#
# Usage:
#   enroll-rig.sh <rig-ssh> <server-ip> [server-host] [admin-email] [admin-pass]
# e.g.
#   enroll-rig.sh rig1-central 10.0.0.99
#
# NOTE on the deploy (pull) path: enrolment/auth/inventory use Mender's STABLE
# device API and work with the Debian mender-client 3.4.0. The deployments
# update-check (`/api/devices/v2/deployments/.../next`) changed shape in the 4.x
# client; server v4.0.1 + client 3.4.0 mismatch there (the rig enrols + reports
# but won't PULL until it runs the 4.x mender-update client). See README.md.
set -euo pipefail

RIG="${1:?usage: enroll-rig.sh <rig-ssh> <server-ip> [host] [admin-email] [admin-pass]}"
SERVER_IP="${2:?server IP required}"
SERVER_HOST="${3:-docker.mender.io}"
ADMIN_EMAIL="${4:-admin@docker.mender.io}"
ADMIN_PASS="${5:-password123}"
BASE="https://$SERVER_IP"

echo "[enroll] $RIG → $SERVER_HOST ($SERVER_IP)"

# --- 1. fetch the server CA (self-signed cert == its own CA) ---
# SERVER_SSH = the ssh alias of the host running the Mender server (default dalek).
SERVER_SSH="${SERVER_SSH:-dalek}"
ca="$(mktemp)"
ssh "$SERVER_SSH" 'cat ~/mender-server/compose/certs/mender.crt' > "$ca"
[ -s "$ca" ] || { echo "could not fetch server CA from $SERVER_SSH"; exit 1; }

# --- 2-4. ship CA + hosts + mender.conf + identity/inventory to the rig ---
scp "$ca" "$RIG:/tmp/mender-server-ca.crt" >/dev/null
ssh "$RIG" "sudo sh -s" <<EOF
set -e
grep -q "$SERVER_HOST" /etc/hosts || echo "$SERVER_IP $SERVER_HOST s3.$SERVER_HOST" >> /etc/hosts
install -d -m0755 /etc/mender
cp /tmp/mender-server-ca.crt /etc/mender/server.crt
cp /tmp/mender-server-ca.crt /usr/local/share/ca-certificates/mender-server.crt
update-ca-certificates >/dev/null 2>&1 || true
cat > /etc/mender/mender.conf <<CONF
{
  "ServerURL": "https://$SERVER_HOST",
  "ServerCertificate": "/etc/mender/server.crt",
  "TenantToken": "",
  "InventoryPollIntervalSeconds": 30,
  "UpdatePollIntervalSeconds": 30,
  "RetryPollIntervalSeconds": 30
}
CONF
install -d -m0755 /usr/share/mender/identity /usr/share/mender/inventory
cat > /usr/share/mender/identity/mender-device-identity <<'ID'
#!/bin/sh
mac=\$(cat /sys/class/net/eth0/address 2>/dev/null || cat /sys/class/net/*/address 2>/dev/null | head -1)
echo "mac=\$mac"
ID
chmod 755 /usr/share/mender/identity/mender-device-identity
cat > /usr/share/mender/inventory/mender-inventory-theia <<'INV'
#!/bin/sh
echo "device_type=theia-rig"
echo "hostname=\$(hostname)"
echo "kernel=\$(uname -r)"
echo "ipv4_eth0=\$(ip -4 -o addr show eth0 2>/dev/null | awk '{print \$4}' | head -1)"
INV
chmod 755 /usr/share/mender/inventory/mender-inventory-theia
# device identity files mender install / auth also need
[ -f /var/lib/mender/device_type ] || { install -d -m0755 /var/lib/mender; echo device_type=theia-rig > /var/lib/mender/device_type; }
[ -f /etc/mender/artifact_info ] || echo artifact_name=unprovisioned > /etc/mender/artifact_info
EOF

# --- 5. bootstrap + start → submit auth request ---
echo "[enroll] bootstrap + start the client (submits the auth request)"
ssh "$RIG" 'sudo mender bootstrap --forcebootstrap >/dev/null 2>&1 || true; sudo systemctl restart mender-client'
sleep 6

# --- 6. accept the pending device via the server device-auth API ---
jwt="$(ssh "$SERVER_SSH" "curl -sk -u '$ADMIN_EMAIL:$ADMIN_PASS' -X POST '$BASE/api/management/v1/useradm/auth/login'")"
pend="$(ssh "$SERVER_SSH" "curl -sk -H 'Authorization: Bearer $jwt' '$BASE/api/management/v2/devauth/devices?status=pending'")"
dev="$(echo "$pend"  | python3 -c 'import sys,json;d=json.load(sys.stdin);print(d[0]["id"] if d else "")')"
aset="$(echo "$pend" | python3 -c 'import sys,json;d=json.load(sys.stdin);print(d[0]["auth_sets"][0]["id"] if d else "")')"
[ -n "$dev" ] || { echo "[enroll] no pending device — check the client logs on $RIG"; exit 1; }
echo "[enroll] accepting device $dev"
ssh "$SERVER_SSH" "curl -sk -H 'Authorization: Bearer $jwt' -H 'Content-Type: application/json' -X PUT '$BASE/api/management/v2/devauth/devices/$dev/auth/$aset/status' -d '{\"status\":\"accepted\"}'"

# --- 7. restart → authorize + submit inventory ---
ssh "$RIG" 'sudo systemctl restart mender-client'
echo "[enroll] done. $RIG is accepted; it will report inventory within ~30s."
echo "[enroll] confirm in the UI: $BASE  ($ADMIN_EMAIL)"
rm -f "$ca"

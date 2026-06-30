#!/bin/sh
# deploy/rig/entrypoint.sh — make the container behave like a real rig, start the
# Mender OTA agent, then exec sshd (passing the per-rig -p <port> through).
set -e

hn="$(hostname)"

# 1. hostname → /etc/hosts (host-net containers lack it, so colony's sudo fails
#    "unable to resolve host <name>"), plus the Mender server vhost (the cert CN
#    is docker.mender.io, served at MENDER_SERVER_IP — default the dalek server).
if ! grep -q "[[:space:]]$hn\$" /etc/hosts 2>/dev/null; then
    echo "127.0.1.1 $hn" >> /etc/hosts
fi
msrv="${MENDER_SERVER_IP:-10.0.0.99}"
if ! grep -q "docker.mender.io" /etc/hosts 2>/dev/null; then
    echo "$msrv docker.mender.io s3.docker.mender.io" >> /etc/hosts
fi

# 2. Device identity. The mender identity script reads /etc/mender/device-id; seed
#    it from the hostname so central/compute are DISTINCT (host-net containers
#    share the host MAC). colony set-identity later overwrites this with the
#    GS-minted UUID — both work; whatever is here is the device's mender identity.
[ -s /etc/mender/device-id ] || echo "$hn" > /etc/mender/device-id

# 3. DBus system bus — mender-auth and mender-update (4.x/5.x) talk over it; a
#    bare container has no system bus, so start one (idempotent).
mkdir -p /run/dbus
[ -e /run/dbus/system_bus_socket ] || dbus-daemon --system --fork 2>/dev/null || true

# 4. Start the Mender OTA agent under a keep-alive loop (mender-auth can exit
#    when the server rejects it before accept; it must come back so the device
#    re-tries after the operator accepts). mender-update is the deployment poller.
#    The agent checks into the server → an accepted device receives deployments.
sleep 1
( while true; do mender-auth daemon >>/var/log/mender-auth.log 2>&1; sleep 5; done ) &
sleep 1
( while true; do mender-update daemon >>/var/log/mender-update.log 2>&1; sleep 5; done ) &

# 4. sshd in the foreground (the container's PID-1 process). -p from compose.
exec /usr/sbin/sshd -D -e "$@"

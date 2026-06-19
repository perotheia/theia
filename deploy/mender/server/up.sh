#!/usr/bin/env bash
# deploy/mender/server/up.sh — stand up a LOCAL Mender server (the VUCM OTA
# transport) from the open-source mendersoftware/mender-server repo, on a fleet
# host (we use dalek). The harness (deploy/vucm/fleet.py) then drives it over the
# Management API: upload a theia-release artifact, deploy to a device group; the
# rig's mender client pulls it → the theia-release module + state-script → UCM.
#
# The OSS Mender server isn't a single image — it's a multi-service compose
# (deployments/deviceauth/inventory/useradm/gui/workflows + mongo/nats/traefik).
# This clones it at a pinned tag and runs ITS `docker compose up -d`. Opt-in:
# needs docker + ~2-3GB + network. This is the next-release server team's surface;
# the harness proves the wire today.
#
# Usage (on the fleet host, e.g. dalek):
#   up.sh up                    # clone (if needed) + compose up -d
#   up.sh user <email> <pass>   # create the initial admin
#   up.sh token <email> <pass>  # mint a PAT, print `export MENDER_TOKEN=…`
#   up.sh down                  # compose down
#
# The API gateway listens on https://localhost (self-signed). From another host,
# point fleet.py at https://<fleet-host> --insecure (or add docker.mender.io to
# /etc/hosts → the fleet host IP, the cert's vhost).
set -euo pipefail

MENDER_SERVER_REF="${MENDER_SERVER_REF:-v4.0.1}"
WORK="${MENDER_SERVER_DIR:-$HOME/mender-server}"
REPO="https://github.com/mendersoftware/mender-server.git"

ACTION="${1:-up}"

_clone() {
    if [ ! -d "$WORK/.git" ]; then
        echo "[server] clone $REPO @ $MENDER_SERVER_REF → $WORK"
        git clone --depth 1 -b "$MENDER_SERVER_REF" "$REPO" "$WORK"
    fi
}

case "$ACTION" in
  up)
    _clone
    # Install the theia override (traefik fix for Docker>=25 hosts) + the static
    # routes (routing independent of the broken Docker-label discovery).
    HERE="$(cd "$(dirname "$0")" && pwd)"
    cp "$HERE/docker-compose.override.yml" "$WORK/docker-compose.override.yml"
    cp "$HERE/theia-routes.yaml" "$WORK/compose/config/traefik/theia-routes.yaml"
    cd "$WORK"
    echo "[server] docker compose up -d (this pulls ~2-3GB on first run)"
    docker compose up -d
    echo "[server] up. API gateway: https://localhost (self-signed cert, vhost docker.mender.io)"
    echo "[server] next: up.sh user <email> <pass>"
    ;;

  user)
    email="${2:?usage: up.sh user <email> <pass>}"; pass="${3:?password required}"
    cd "$WORK"
    docker compose exec -T useradm useradm create-user --username "$email" --password "$pass"
    echo "[server] admin '$email' created"
    ;;

  token)
    # Mint a Personal Access Token for fleet.py: log in for a JWT, then POST a PAT.
    email="${2:?usage: up.sh token <email> <pass>}"; pass="${3:?password required}"
    base="${MENDER_SERVER:-https://localhost}"
    jwt="$(curl -sk -u "$email:$pass" -X POST "$base/api/management/v1/useradm/auth/login")"
    [ -n "$jwt" ] || { echo "login failed" >&2; exit 1; }
    pat="$(curl -sk -H "Authorization: Bearer $jwt" -H "Content-Type: application/json" \
        -X POST "$base/api/management/v1/useradm/settings/tokens" \
        -d '{"name":"theia-fleet","expires_in":7776000}')"
    # The PAT is the response body (a raw JWT string for v4).
    echo "[server] export MENDER_TOKEN=$(echo "$pat" | tr -d '\"')"
    ;;

  down)
    cd "$WORK"; docker compose down ;;

  *)
    echo "usage: up.sh {up|user|token|down}" >&2; exit 2 ;;
esac

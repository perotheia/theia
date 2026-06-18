#!/usr/bin/env bash
# tools/gen_dev_certs.sh — generate a self-signed dev CA + a com server cert for
# local TLS testing of com's gRPC endpoints (crypto/TLS plan phase 1).
#
# These are DEV-ONLY certs (self-signed, no real PKI). Point com at them with:
#   THEIA_COM_TLS_CERT=<out>/server.crt
#   THEIA_COM_TLS_KEY=<out>/server.key
#   THEIA_COM_TLS_CA=<out>/ca.crt        # optional (client-cert verification)
# and rtdb/GUI at:
#   THEIA_COM_TLS_CA=<out>/ca.crt        # so the client trusts the dev CA
#
# Usage: tools/gen_dev_certs.sh [out_dir] [server_cn] [extra_sans]
#   out_dir     default ./dev-certs
#   server_cn   default localhost  (must match the host clients dial)
#   extra_sans  extra SAN entries (comma list of "DNS:x"/"IP:x"), so ONE cert set
#               works for GUI dev (localhost/127.0.0.1) AND test rigs by IP, e.g.
#               tools/gen_dev_certs.sh dist/manifest/central/certs localhost \
#                   "IP:10.0.0.22,IP:192.168.50.213,DNS:rig1-central"
#               Or set THEIA_DEV_CERT_SANS in the env (same comma format).
set -euo pipefail

OUT="${1:-dev-certs}"
CN="${2:-localhost}"
EXTRA_SANS="${3:-${THEIA_DEV_CERT_SANS:-}}"
DAYS=825   # < 825 keeps macOS/modern clients happy; dev anyway

mkdir -p "$OUT"
cd "$OUT"

echo "[gen_dev_certs] writing dev CA + com server cert to $(pwd) (CN=$CN)"

# --- CA -------------------------------------------------------------------
openssl genrsa -out ca.key 4096 2>/dev/null
openssl req -x509 -new -nodes -key ca.key -sha256 -days "$DAYS" \
    -subj "/O=Theia Dev/CN=Theia Dev CA" -out ca.crt 2>/dev/null

# --- com server cert (signed by the dev CA) -------------------------------
# SAN must carry the CN + 127.0.0.1 so gRPC's hostname check passes on a
# 127.0.0.1 dial (rtdb's default target).
openssl genrsa -out server.key 4096 2>/dev/null
openssl req -new -key server.key -subj "/O=Theia Dev/CN=$CN" -out server.csr 2>/dev/null

# SAN = the CN + localhost/127.0.0.1 (GUI dev dials those) + any extra rig SANs
# (so the SAME cert validates when rtdb/GUI dial a test rig by IP). A trailing
# ", $EXTRA_SANS" only when extras were given.
cat > server.ext <<EXT
subjectAltName = DNS:$CN, DNS:localhost, IP:127.0.0.1${EXTRA_SANS:+, $EXTRA_SANS}
extendedKeyUsage = serverAuth
EXT

openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
    -days "$DAYS" -sha256 -extfile server.ext -out server.crt 2>/dev/null

# --- client cert (signed by the dev CA) -----------------------------------
# MUTUAL auth: com REQUIRES + VERIFIES a client cert when a CA is configured,
# so the GUI/rtdb present this. clientAuth EKU; CN identifies the client. No SAN
# needed (clients aren't dialled by name), but harmless to omit.
openssl genrsa -out client.key 4096 2>/dev/null
openssl req -new -key client.key -subj "/O=Theia Dev/CN=theia-client" \
    -out client.csr 2>/dev/null

cat > client.ext <<EXT
extendedKeyUsage = clientAuth
EXT

openssl x509 -req -in client.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
    -days "$DAYS" -sha256 -extfile client.ext -out client.crt 2>/dev/null

rm -f server.csr server.ext client.csr client.ext ca.srl

echo "[gen_dev_certs] done:"
echo "  CA cert:     $(pwd)/ca.crt"
echo "  server cert: $(pwd)/server.crt    server key: $(pwd)/server.key"
echo "  client cert: $(pwd)/client.crt    client key: $(pwd)/client.key"
echo
echo "Run com with MUTUAL TLS (requires + verifies the client cert):"
echo "  THEIA_COM_TLS_CERT=$(pwd)/server.crt \\"
echo "  THEIA_COM_TLS_KEY=$(pwd)/server.key \\"
echo "  THEIA_COM_TLS_CA=$(pwd)/ca.crt ./supervisor ..."
echo "Point rtdb/GUI at the CA + present the client identity:"
echo "  THEIA_COM_TLS_CA=$(pwd)/ca.crt \\"
echo "  THEIA_COM_TLS_CLIENT_CERT=$(pwd)/client.crt \\"
echo "  THEIA_COM_TLS_CLIENT_KEY=$(pwd)/client.key rtdb ps"

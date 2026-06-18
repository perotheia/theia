#!/usr/bin/env bash
# deploy/vucm/campaign.sh — the VUCM (fleet/Mender-server) campaign driver.
#
# Pushes ONE update to the on-device UCM agent (services/ucm). In a real fleet
# this is a Mender deployment to a device group; in this self-hosted demo it
# reaches the agent through the artheia probe (UcmDaemon.RequestUpdate over TIPC)
# — the same transport-agnostic client tdb uses. (A com UcmView gRPC proxy is the
# production path; the probe is the minimal demo driver.)
#
# Usage:
#   campaign.sh sw   <version> [full|partial] [fc]   # Software Package
#   campaign.sh conf <version>                       # Configuration Package
#
# Examples:
#   campaign.sh sw   2.0.0 full            # full platform upgrade v1→v2
#   campaign.sh sw   2.1.0 partial phm     # partial: just the phm FC
#   campaign.sh conf 1.1.0                 # config bump → etcd via per
set -euo pipefail

KIND="${1:?usage: campaign.sh sw|conf <version> [full|partial] [fc]}"
VERSION="${2:?version required}"
SCOPE="${3:-full}"
FC="${4:-theia}"

ROOT="${THEIA_ROOT:-/opt/theia}"
ARTIFACTS="$ROOT/artifacts"
PKG="$ARTIFACTS/theia-${FC}-${VERSION}.tar.zst"

# Map the CLI to the PackageManifest enum ints (UpdateKind / UpdateScope).
case "$KIND" in
  sw)   uk=0 ;;
  conf) uk=1 ;;
  *) echo "kind must be sw|conf" >&2; exit 2 ;;
esac
case "$SCOPE" in
  full)    us=0 ;;
  partial) us=1 ;;
  *) echo "scope must be full|partial" >&2; exit 2 ;;
esac

echo "[vucm] campaign: kind=$KIND version=$VERSION scope=$SCOPE fc=$FC artifact=$PKG" >&2

# Drive UcmDaemon.RequestUpdate via the probe (artheia). The ucm .art declares
# UpdateCtl.RequestUpdate(PackageManifest). We materialise a one-shot client node
# bound to UcmDaemon's address and CALL it.
exec python3 - "$ROOT" "$FC" "$VERSION" "$uk" "$us" "$PKG" <<'PY'
import sys
root, name, version, uk, us, artifact = sys.argv[1:7]
sys.path.insert(0, f"{root}/artheia")
sys.path.insert(0, f"{root}/tools/tdb")
from artheia.gen_server.probe import ArtheiaContext
# The ucm package .art (canonical symlink path) — resolves UcmDaemon + UpdateCtl.
ctx = ArtheiaContext(f"{root}/system/services/ucm/package.art",
                     proto_root=f"{root}/platform/proto")
probe = ctx.probe("UcmDaemon").start()   # impersonate a caller of UcmDaemon
try:
    rep = probe.call("UcmDaemon", "RequestUpdate", timeout=8.0,
                     name=name, version=version,
                     kind=int(uk), scope=int(us),
                     artifact_path=artifact, signature="",
                     has_migrations=False)
    status = rep.get("status") if isinstance(rep, dict) else getattr(rep, "status", "?")
    print(f"[vucm] UCM accepted={status==0} (status={status})", file=sys.stderr)
    sys.exit(0 if status == 0 else 1)
finally:
    probe.stop()
PY

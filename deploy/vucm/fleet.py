#!/usr/bin/env python3
"""deploy/vucm/fleet.py — the VUCM fleet-OTA spine (Mender Management API client).

VUCM = the fleet/management side of AUTOSAR software management (vs UCM = the
on-device agent, services/ucm). Its OTA transport is a **Mender server**: you
upload a `theia-release` artifact, create a *deployment* to a device *group*, and
the rig's mender client pulls it — the theia-release update module lands the
release-dir + symlink, and the ArtifactInstall_Leave state-script hands off to UCM.

This is the MINIMAL, REUSABLE harness. The real management server (next release)
is a service that owns campaigns, approval, phased rollout, dependency graphs —
but it talks to Mender through these SAME Management API calls. So this file is the
spine: a thin, dependency-light client (urllib only) over the endpoints the real
server will reuse, plus a CLI to drive the loop by hand today.

Concept map (where this sits):
    rig-enroll  (tools/rig-enroll)  → identity/PKI/VPN over com gRPC   [day-0]
    fleet.py    (this)              → OTA: upload+deploy via Mender API [day-2, field]
    campaign.sh (deploy/vucm)       → direct UCM trigger over probe     [self-hosted demo]
    UCM agent   (services/ucm)      → on-device lifecycle either delivers a release into

Mender Management API (paths verified against the OSS server v4.0.1 on dalek):
    POST /api/management/v1/deployments/artifacts             upload a .mender
    GET  /api/management/v1/deployments/artifacts             list artifacts
    POST /api/management/v1/deployments/deployments           create a deployment
    GET  /api/management/v1/deployments/deployments/{id}      deployment status
    GET  /api/management/v1/deployments/deployments/{id}/statistics  rollup
    GET  /api/management/v1/inventory/devices                 device groups
    POST /api/management/v1/useradm/auth/login                JWT (basic-auth)
    POST /api/management/v1/useradm/settings/tokens           mint a PAT

Note: hosted Mender (hosted.mender.io) uses the v2 artifact/deployment paths; the
OSS v4 server nests them under /deployments/. The DEPLOY_BASE/ARTIFACT_BASE below
default to the OSS v4 layout; override via --api-flavor hosted if you target SaaS.

Auth: a Personal Access Token (PAT) minted in the Mender UI / `useradm`. Pass via
--token or $MENDER_TOKEN. Server URL via --server or $MENDER_SERVER
(default https://localhost — the local compose server).

Usage:
    fleet.py upload   <artifact.mender>
    fleet.py artifacts
    fleet.py deploy   <artifact-name> <device-group> [--name <deployment-name>]
    fleet.py status   <deployment-id>
    fleet.py devices  [--group <g>]
    fleet.py release  <version> <release-dir> <device-group>   # build+upload+deploy

`release` is the one-shot the real server's "roll out version X to group G" maps
onto: it shells out to deploy/mender/build-artifact.sh, uploads, and deploys.
"""
import argparse
import json
import os
import ssl
import subprocess
import sys
import urllib.error
import urllib.request
import uuid
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILD_ARTIFACT = HERE.parent / "mender" / "build-artifact.sh"

# --- the management-API surface (the spine the real server reuses) -----------

class Mender:
    """Thin Mender Management API client. urllib only — no requests dep, so it
    drops into the framework image / CI without extra packages."""

    # The OSS v4 server nests the deployments plane under /deployments/; hosted
    # Mender flattens it. Pick the base prefixes by flavor.
    BASES = {
        "oss":    {"art": "/api/management/v1/deployments/artifacts",
                   "dep": "/api/management/v1/deployments/deployments"},
        "hosted": {"art": "/api/management/v2/deployments/artifacts",
                   "dep": "/api/management/v1/deployments"},
    }

    def __init__(self, server: str, token: str, insecure: bool = False,
                 flavor: str = "oss"):
        self.server = server.rstrip("/")
        self.token = token
        self.art = self.BASES[flavor]["art"]
        self.dep = self.BASES[flavor]["dep"]
        # The local compose server uses a self-signed cert; --insecure skips
        # verification (dev only — the real server pins a CA).
        self._ctx = ssl._create_unverified_context() if insecure else None

    def _req(self, method: str, path: str, *, body=None, ctype=None,
             raw: bytes | None = None) -> tuple[int, bytes, dict]:
        url = f"{self.server}{path}"
        data = raw if raw is not None else (
            json.dumps(body).encode() if body is not None else None)
        req = urllib.request.Request(url, data=data, method=method)
        req.add_header("Authorization", f"Bearer {self.token}")
        if ctype:
            req.add_header("Content-Type", ctype)
        elif body is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, context=self._ctx) as r:
                return r.status, r.read(), dict(r.headers)
        except urllib.error.HTTPError as e:
            return e.code, e.read(), dict(e.headers)

    # ---- artifacts ----
    def upload_artifact(self, mender_file: Path, description: str = "") -> str:
        """POST a .mender as multipart/form-data. Returns the Location id."""
        boundary = f"----theia{uuid.uuid4().hex}"
        blob = mender_file.read_bytes()
        parts = []
        if description:
            parts += [f"--{boundary}".encode(),
                      b'Content-Disposition: form-data; name="description"', b"",
                      description.encode()]
        parts += [
            f"--{boundary}".encode(),
            (f'Content-Disposition: form-data; name="artifact"; '
             f'filename="{mender_file.name}"').encode(),
            b"Content-Type: application/octet-stream", b"", blob,
            f"--{boundary}--".encode(), b"",
        ]
        payload = b"\r\n".join(parts)
        st, data, hdrs = self._req(
            "POST", self.art,
            raw=payload, ctype=f"multipart/form-data; boundary={boundary}")
        if st not in (201, 200):
            raise RuntimeError(f"upload failed [{st}]: {data.decode(errors='replace')[:300]}")
        loc = hdrs.get("Location", "")
        return loc.rstrip("/").split("/")[-1]

    def list_artifacts(self) -> list:
        st, data, _ = self._req("GET", f"{self.art}?per_page=200")
        if st != 200:
            raise RuntimeError(f"list artifacts [{st}]: {data.decode(errors='replace')[:200]}")
        return json.loads(data or b"[]")

    # ---- deployments ----
    def create_deployment(self, name: str, artifact_name: str,
                          devices: list[str]) -> str:
        """Create a deployment of <artifact_name> to an explicit device-id list.
        (Group rollouts use create_deployment_for_group below — the v1 API takes a
        device list; a group is resolved to its devices by the caller.)"""
        st, data, hdrs = self._req(
            "POST", self.dep,
            body={"name": name, "artifact_name": artifact_name, "devices": devices})
        if st != 201:
            raise RuntimeError(f"create deployment [{st}]: {data.decode(errors='replace')[:300]}")
        return hdrs.get("Location", "").rstrip("/").split("/")[-1]

    def deployment_status(self, dep_id: str) -> dict:
        st, data, _ = self._req("GET", f"{self.dep}/{dep_id}")
        if st != 200:
            raise RuntimeError(f"status [{st}]: {data.decode(errors='replace')[:200]}")
        return json.loads(data or b"{}")

    def deployment_statistics(self, dep_id: str) -> dict:
        st, data, _ = self._req("GET", f"{self.dep}/{dep_id}/statistics")
        if st != 200:
            raise RuntimeError(f"statistics [{st}]: {data.decode(errors='replace')[:200]}")
        return json.loads(data or b"{}")

    # ---- inventory (device groups) ----
    def devices(self, group: str | None = None) -> list:
        path = "/api/management/v1/inventory/devices?per_page=500"
        if group:
            path += f"&group={group}"
        st, data, _ = self._req("GET", path)
        if st != 200:
            raise RuntimeError(f"devices [{st}]: {data.decode(errors='replace')[:200]}")
        return json.loads(data or b"[]")

    def device_ids_in_group(self, group: str) -> list[str]:
        return [d["id"] for d in self.devices(group)]


# --- CLI ----------------------------------------------------------------------

def _client(args) -> Mender:
    server = args.server or os.environ.get("MENDER_SERVER", "https://localhost")
    token = args.token or os.environ.get("MENDER_TOKEN", "")
    if not token:
        sys.exit("no token: pass --token or set MENDER_TOKEN (a Mender PAT)")
    return Mender(server, token, insecure=args.insecure, flavor=args.api_flavor)


def cmd_upload(args) -> int:
    m = _client(args)
    aid = m.upload_artifact(Path(args.artifact), args.description)
    print(f"[fleet] uploaded {args.artifact} → artifact id {aid}")
    return 0


def cmd_artifacts(args) -> int:
    m = _client(args)
    for a in m.list_artifacts():
        print(f"  {a.get('name'):20} {a.get('device_types_compatible')}  "
              f"size={a.get('size')}  id={a.get('id')}")
    return 0


def cmd_deploy(args) -> int:
    m = _client(args)
    devices = m.device_ids_in_group(args.group)
    if not devices:
        sys.exit(f"no devices in group '{args.group}' (is the rig enrolled + grouped?)")
    name = args.name or f"theia-{args.artifact_name}-{args.group}"
    dep_id = m.create_deployment(name, args.artifact_name, devices)
    print(f"[fleet] deployment '{name}' → {len(devices)} device(s) in '{args.group}': id {dep_id}")
    return 0


def cmd_status(args) -> int:
    m = _client(args)
    dep = m.deployment_status(args.deployment_id)
    stats = m.deployment_statistics(args.deployment_id)
    print(f"[fleet] {dep.get('name')}: {dep.get('status')}  artifact={dep.get('artifact_name')}")
    print(f"        stats: {json.dumps(stats)}")
    return 0


def cmd_devices(args) -> int:
    m = _client(args)
    for d in m.devices(args.group):
        attrs = {a["name"]: a["value"] for a in d.get("attributes", [])}
        print(f"  {d['id']}  group={attrs.get('group','-')}  "
              f"device_type={attrs.get('device_type','-')}")
    return 0


def cmd_release(args) -> int:
    """The one-shot the real server's 'roll out <version> to <group>' maps onto:
    build the theia-release artifact, upload it, deploy it to the group."""
    out = Path(f"/tmp/theia-{args.version}.mender")
    print(f"[fleet] build {args.version} from {args.release_dir} → {out}")
    rc = subprocess.call([str(BUILD_ARTIFACT), args.version, args.release_dir,
                          args.device_type, str(out)])
    if rc != 0:
        return rc
    m = _client(args)
    m.upload_artifact(out, f"theia release {args.version}")
    print(f"[fleet] uploaded {args.version}")
    devices = m.device_ids_in_group(args.group)
    if not devices:
        sys.exit(f"no devices in group '{args.group}'")
    dep_id = m.create_deployment(f"theia-{args.version}-{args.group}", args.version, devices)
    print(f"[fleet] deployed {args.version} → '{args.group}' ({len(devices)} dev): id {dep_id}")
    print(f"[fleet] track:  fleet.py status {dep_id}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description="VUCM fleet-OTA via the Mender Management API")
    p.add_argument("--server", help="Mender server URL ($MENDER_SERVER)")
    p.add_argument("--token", help="Mender PAT ($MENDER_TOKEN)")
    p.add_argument("--insecure", action="store_true",
                   help="skip TLS verify (local self-signed compose server)")
    p.add_argument("--api-flavor", choices=["oss", "hosted"], default="oss",
                   help="API path layout: oss (v4 self-hosted, default) or hosted (SaaS)")
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("upload"); sp.add_argument("artifact")
    sp.add_argument("--description", default=""); sp.set_defaults(fn=cmd_upload)

    sp = sub.add_parser("artifacts"); sp.set_defaults(fn=cmd_artifacts)

    sp = sub.add_parser("deploy")
    sp.add_argument("artifact_name"); sp.add_argument("group")
    sp.add_argument("--name"); sp.set_defaults(fn=cmd_deploy)

    sp = sub.add_parser("status"); sp.add_argument("deployment_id")
    sp.set_defaults(fn=cmd_status)

    sp = sub.add_parser("devices"); sp.add_argument("--group")
    sp.set_defaults(fn=cmd_devices)

    sp = sub.add_parser("release")
    sp.add_argument("version"); sp.add_argument("release_dir"); sp.add_argument("group")
    sp.add_argument("--device-type", default="theia-rig")
    sp.set_defaults(fn=cmd_release)

    args = p.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())

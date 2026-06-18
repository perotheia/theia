#!/usr/bin/env python3
"""theia rig-enroll — centralized rig enrollment over com.

The theia-native analog of mosaic's `moz orchestrate cloud_provision`: a fleet
manager (colocated with Headscale, e.g. on dalek) provisions a rig's identity
(UUID/VIN) + PKI creds + an optional Headscale VPN preauth key — but over com
gRPC (:7700) instead of nested SSH. com writes the files under
/etc/theia/manifest/<machine>/ on the rig; the rig can then serve mutual-TLS com
and join the VPN.

  Enrollment utility ──gRPC──> com on the rig   (Provision: uuid/vin/pki/authkey)
        │
        └──REST──> Headscale (local)            (mint preauth key, create user)

Config (enroll.json), mirroring mosaic's cloud_provision config:
  {
    "machine":     "rig1-central",
    "target":      "10.0.0.22:7700",       # com gRPC endpoint (eth; or wifi to recover)
    "vehicleUuid": "3ebdab5d-...",
    "vehicleVin":  "WP0ZZZ99ZTS392124",
    "clientKey":   "-----BEGIN PRIVATE KEY-----\n...",   # optional → omit to skip
    "clientCert":  "-----BEGIN CERTIFICATE-----\n...",
    "serverCaTrustChain": "-----BEGIN CERTIFICATE-----\n...",
    "vpn": { "enroll": true, "user": "rig", "expiration": "24h" }   # optional
  }

Usage:
  rig_enroll.py enroll  enroll.json [--no-vpn]
  rig_enroll.py status  --target 10.0.0.22:7700
  rig_enroll.py recover enroll.json     # same as enroll but via the wifi target
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
# Reuse rtdb's generated proto stubs (same supervisor_bridge.proto).
sys.path.insert(0, str(REPO / "tools" / "rtdb" / "_gen"))
sys.path.insert(0, str(HERE))

import grpc  # noqa: E402
import supervisor_bridge_pb2 as br  # noqa: E402
import supervisor_bridge_pb2_grpc as brg  # noqa: E402


def _channel(target: str) -> "grpc.Channel":
    """gRPC channel to the rig's com. TLS when a CA is pinned (post-enrollment),
    else insecure — FIRST enrollment is necessarily over an insecure/LAN channel
    (the rig has no cert yet; that's exactly what we're delivering)."""
    ca = os.environ.get("THEIA_COM_TLS_CA", "")
    if ca and os.path.isfile(ca):
        with open(ca, "rb") as f:
            creds = grpc.ssl_channel_credentials(root_certificates=f.read())
        return grpc.secure_channel(target, creds)
    return grpc.insecure_channel(target)


def _load(cfg_path: str) -> dict:
    p = Path(cfg_path)
    if not p.is_file():
        sys.exit(f"config not found: {p}")
    return json.loads(p.read_text())


def _maybe_mint_vpn_key(cfg: dict, do_vpn: bool) -> str:
    """If the config asks for VPN enrollment, mint a Headscale preauth key."""
    vpn = cfg.get("vpn") or {}
    if not do_vpn or not vpn.get("enroll"):
        return ""
    try:
        from headscale_client import HeadscaleClient
    except ImportError:
        print("  ! headscale_client unavailable / requests missing — skipping VPN key")
        return ""
    try:
        hs = HeadscaleClient()
        key = hs.create_preauthkey(
            user=vpn.get("user", "rig"),
            reusable=vpn.get("reusable", True),
            expiration=vpn.get("expiration", "24h"),
        )
        print(f"  minted Headscale preauth key (user={vpn.get('user', 'rig')})")
        return key
    except Exception as e:  # noqa: BLE001
        print(f"  ! Headscale key mint failed: {e}")
        return ""


def cmd_enroll(args, target_override: str = "") -> int:
    cfg = _load(args.config)
    target = target_override or args.target or cfg.get("target", "")
    if not target:
        sys.exit("no com target (config 'target' or --target)")

    authkey = _maybe_mint_vpn_key(cfg, not args.no_vpn)

    req = br.ProvisionRequest(
        machine=cfg.get("machine", ""),
        uuid=cfg.get("vehicleUuid", ""),
        vin=cfg.get("vehicleVin", ""),
        client_key=cfg.get("clientKey", ""),
        client_cert=cfg.get("clientCert", ""),
        ca_chain=cfg.get("serverCaTrustChain", ""),
        vpn_authkey=authkey,
    )
    print(f"enrolling machine={cfg.get('machine', '(default)')} via com {target}")
    ch = _channel(target)
    try:
        stub = brg.ProvisioningStub(ch)
        rep = stub.Provision(req, timeout=15.0)
    except grpc.RpcError as e:
        print(f"  FAILED: {e.code().name} — {e.details()}")
        return 1
    finally:
        ch.close()

    print(f"  ok={rep.ok}  {rep.message}")
    for w in rep.written:
        print(f"    wrote {w}")
    return 0 if rep.ok else 1


def cmd_status(args) -> int:
    if not args.target:
        sys.exit("--target required (com gRPC endpoint)")
    ch = _channel(args.target)
    try:
        stub = brg.ProvisioningStub(ch)
        rep = stub.GetProvisionStatus(br.ProvisionStatusRequest(), timeout=8.0)
    except grpc.RpcError as e:
        print(f"FAILED: {e.code().name} — {e.details()}")
        return 1
    finally:
        ch.close()
    print(f"machine={rep.machine}")
    print(f"  uuid={rep.uuid or '(unset)'}")
    print(f"  vin={rep.vin or '(unset)'}")
    print(f"  client.key={'yes' if rep.has_client_key else 'no'} "
          f"client.crt={'yes' if rep.has_client_cert else 'no'} "
          f"server_ca.crt={'yes' if rep.has_ca_chain else 'no'} "
          f"vpn.authkey={'yes' if rep.has_vpn_authkey else 'no'}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(prog="rig-enroll", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    pe = sub.add_parser("enroll", help="provision a rig over com (eth target)")
    pe.add_argument("config")
    pe.add_argument("--target", default="", help="com gRPC endpoint (host:7700)")
    pe.add_argument("--no-vpn", action="store_true", help="skip the VPN preauth key")

    pr = sub.add_parser("recover", help="re-enroll a rig over its WIFI target "
                        "(when eth is down) — config 'recover_target' or --target")
    pr.add_argument("config")
    pr.add_argument("--target", default="", help="wifi com endpoint")
    pr.add_argument("--no-vpn", action="store_true")

    ps = sub.add_parser("status", help="what a rig already has provisioned")
    ps.add_argument("--target", required=True, help="com gRPC endpoint")

    args = ap.parse_args()
    if args.cmd == "enroll":
        return cmd_enroll(args)
    if args.cmd == "recover":
        cfg = _load(args.config)
        tgt = args.target or cfg.get("recover_target", "")
        if not tgt:
            sys.exit("recover needs a wifi target (--target or config 'recover_target')")
        print(f"RECOVERY mode — enrolling over the WIFI path ({tgt})")
        return cmd_enroll(args, target_override=tgt)
    if args.cmd == "status":
        return cmd_status(args)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())

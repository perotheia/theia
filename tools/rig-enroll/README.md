# rig-enroll — centralized rig enrollment over com

The theia-native analog of mosaic's `moz orchestrate cloud_provision`
(`up/mosaic-eng-ref/.../provision/cloud_provision.py`). A **centralized**
enrollment utility provisions a rig's identity (UUID/VIN) + PKI creds + an
optional Tailscale VPN auth key. mosaic does this over nested SSH through a
ground station; **we do it over com gRPC** (`:7700`), so it's not a local debug
tool (that's rtdb) — it runs where the fleet manager lives.

```
Enrollment utility
   ├── gRPC  ───> com on the rig (:7700)        Provision(uuid, vin, pki, authkey)
   └── HTTPS ───> Tailscale API (api.tailscale.com)  mint a device auth key
```

We use the real **Tailscale SaaS** (not a self-hosted Headscale): rig-enroll
mints a per-rig auth key via the Tailscale API and ships it as `vpn.authkey`; the
rig runs `tailscale up --authkey <key>` against the **default** login server.

com writes the files on the rig under `/etc/theia/manifest/<machine>/`:

| file | mode | source |
|---|---|---|
| `uuid` | 0644 | `vehicleUuid` |
| `vin`  | 0644 | `vehicleVin` |
| `certs/client.key` | **0600** | `clientKey` |
| `certs/client.crt` | 0644 | `clientCert` |
| `certs/server_ca.crt` | 0644 | `serverCaTrustChain` |
| `certs/vpn.authkey` | **0600** | auth key minted via the Tailscale API |

These are the SAME paths com TLS + the supervisor read, so a provisioned rig can
immediately serve mutual-TLS com and join the VPN.

## Usage

```sh
# A Tailscale API access token (admin → Settings → Keys → Generate access token).
export TS_API_TOKEN=tskey-api-...
# optional: TS_TAILNET (defaults to '-', the token's own tailnet)

# enroll over the eth path (mints a Tailscale auth key + provisions over com)
python3 tools/rig-enroll/rig_enroll.py enroll  enroll.json

# what a rig already has (idempotency / verify)
python3 tools/rig-enroll/rig_enroll.py status  --target 10.0.0.22:7700

# RECOVERY: if the eth network is down, re-enroll over the rig's WIFI path
# (config "recover_target", e.g. 192.168.50.213:7700)
python3 tools/rig-enroll/rig_enroll.py recover enroll.json
```

Config: see `enroll.example.json`. PKI fields are optional (omit to skip cert
provisioning); `vpn.enroll=true` mints a Tailscale auth key (via `TS_API_TOKEN`)
and ships it as `vpn.authkey`.

## On the rig (after provisioning)

```sh
curl -fsSL https://tailscale.com/install.sh | sh        # once
tailscale up --authkey "$(cat /etc/theia/manifest/<machine>/certs/vpn.authkey)"
# NO --login-server — the real Tailscale SaaS. NM's vpn_observe() then sees the
# tunnel and drives the VPN_ESTABLISHED readiness rung.
```

## The recovery model

A rig is reachable two ways (ssh aliases mirror this):
- `rig1-central`      → eth `10.0.0.22`     (the fleet net, primary)
- `rig1-central-wifi` → wifi `192.168.50.213` (recovery)

com binds `0.0.0.0:7700`, so both paths reach the same Provisioning service.
`enroll` uses the eth `target`; `recover` uses the wifi `recover_target` — so a
rig whose eth link died can still be re-provisioned / recovered over wifi.

## Notes

- First enrollment is over an insecure/LAN channel (the rig has no cert yet —
  that's exactly what we're delivering). Once `server_ca.crt` is set,
  `THEIA_COM_TLS_CA` makes the channel mutual-TLS.
- The proto: `Provisioning` service in `services/com/proto/supervisor_bridge.proto`
  (`Provision` + `GetProvisionStatus`). com handler: `ProvisioningImpl` in
  `services/com/impl/ComGrpcProxy_handlers.cc` (pure file I/O, no TIPC link).
- `tailscale_client.py` is a thin client for the Tailscale API
  (`api.tailscale.com/api/v2`): mint auth keys, list keys, list/delete/expire
  devices, set routes.

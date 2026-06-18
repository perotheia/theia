"""Thin Tailscale management-API client (api.tailscale.com/api/v2).

Replaces headscale_client.py: we dropped the self-hosted Headscale control plane
for the real Tailscale SaaS (the engineer already runs a Tailscale tailnet). The
enrollment utility MINTS a per-rig auth key via the Tailscale API, then ships it
to the rig as vpn.authkey over com — the rig runs `tailscale up --authkey <key>`
against the DEFAULT login server (no --login-server).

Auth: a Tailscale API ACCESS TOKEN (Settings → Keys → Generate access token),
passed via TS_API_TOKEN. HTTP basic auth, token as the username, empty password.
The tailnet is addressed as '-' (the token's own tailnet) unless TS_TAILNET is set.

Docs: https://tailscale.com/api  (POST /tailnet/{tailnet}/keys mints an auth key).
"""

from __future__ import annotations

import os
from typing import Any, Optional

import requests


class TailscaleClient:
    BASE = "https://api.tailscale.com/api/v2"

    def __init__(
        self,
        api_token: Optional[str] = None,
        tailnet: Optional[str] = None,
        timeout: float = 15.0,
    ) -> None:
        self.api_token = api_token or os.environ.get("TS_API_TOKEN", "")
        if not self.api_token:
            raise ValueError(
                "no Tailscale API token (set TS_API_TOKEN or pass api_token); "
                "create one at Tailscale admin → Settings → Keys → Generate access token")
        # '-' is the API alias for the token's own tailnet (works on every plan).
        self.tailnet = tailnet or os.environ.get("TS_TAILNET", "-")
        self.timeout = timeout
        self._s = requests.Session()
        self._s.auth = (self.api_token, "")   # token as basic-auth username

    def _req(self, method: str, path: str, **kw) -> Any:
        r = self._s.request(method, f"{self.BASE}{path}", timeout=self.timeout, **kw)
        r.raise_for_status()
        return r.json() if r.content else {}

    # ── auth keys ──────────────────────────────────────────────────────────
    def create_authkey(
        self,
        reusable: bool = True,
        ephemeral: bool = False,
        preauthorized: bool = True,
        tags: Optional[list[str]] = None,
        expiry_seconds: int = 7776000,   # 90d (Tailscale max for a key)
        description: str = "theia-rig-enroll",
    ) -> str:
        """Mint a device auth key. Returns the tskey-auth-... string (shown once)."""
        body = {
            "capabilities": {"devices": {"create": {
                "reusable": reusable,
                "ephemeral": ephemeral,
                "preauthorized": preauthorized,
                "tags": tags or [],
            }}},
            "expirySeconds": expiry_seconds,
            "description": description,
        }
        resp = self._req("POST", f"/tailnet/{self.tailnet}/keys", json=body)
        return resp.get("key", "")

    def list_keys(self) -> list[dict]:
        return self._req("GET", f"/tailnet/{self.tailnet}/keys").get("keys", [])

    # ── devices (the tailnet's machines) ───────────────────────────────────
    def list_devices(self) -> list[dict]:
        return self._req("GET", f"/tailnet/{self.tailnet}/devices").get("devices", [])

    def device_id(self, hostname: str) -> Optional[str]:
        for d in self.list_devices():
            # `name` is the MagicDNS name (hostname.tailnet.ts.net); `hostname` is bare.
            if d.get("hostname") == hostname or d.get("name", "").startswith(hostname + "."):
                return d.get("id")
        return None

    def delete_device(self, device_id: str) -> Any:
        return self._req("DELETE", f"/device/{device_id}")

    def expire_device_key(self, device_id: str) -> Any:
        return self._req("POST", f"/device/{device_id}/expire")

    def set_device_routes(self, device_id: str, routes: list[str]) -> Any:
        return self._req("POST", f"/device/{device_id}/routes",
                         json={"routes": routes})

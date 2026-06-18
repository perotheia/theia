"""Thin Headscale management-API client (REST /api/v1).

Headscale exposes a gRPC mgmt API on :9090 (loopback) AND a REST API on the
HTTP listener (:8080) behind an API key. We use the REST API — it's reachable
from anywhere the control server is, needs only `requests`, and covers every
enrollment op (create user, mint preauth key, list/expire nodes, set routes).

The API key is created once on the server:
    headscale apikeys create --expiration 90d
and passed here via HEADSCALE_API_KEY (or the constructor).

Docs: https://headscale.net/ (API: the same handlers the `headscale` CLI calls).
"""

from __future__ import annotations

import os
from typing import Any, Optional

import requests


class HeadscaleClient:
    def __init__(
        self,
        base_url: Optional[str] = None,
        api_key: Optional[str] = None,
        timeout: float = 10.0,
    ) -> None:
        self.base_url = (base_url or os.environ.get(
            "HEADSCALE_URL", "http://10.0.0.99:8080")).rstrip("/")
        self.api_key = api_key or os.environ.get("HEADSCALE_API_KEY", "")
        if not self.api_key:
            raise ValueError(
                "no Headscale API key (set HEADSCALE_API_KEY or pass api_key); "
                "create one on the server: headscale apikeys create --expiration 90d")
        self.timeout = timeout
        self._s = requests.Session()
        self._s.headers.update({"Authorization": f"Bearer {self.api_key}"})

    def _req(self, method: str, path: str, **kw) -> Any:
        url = f"{self.base_url}/api/v1{path}"
        r = self._s.request(method, url, timeout=self.timeout, **kw)
        r.raise_for_status()
        return r.json() if r.content else {}

    # ── users ──────────────────────────────────────────────────────────────
    def list_users(self) -> list[dict]:
        return self._req("GET", "/user").get("users", [])

    def create_user(self, name: str) -> dict:
        # Idempotent: return the existing user if already present.
        for u in self.list_users():
            if u.get("name") == name:
                return u
        return self._req("POST", "/user", json={"name": name}).get("user", {})

    def user_id(self, name: str) -> Optional[str]:
        for u in self.list_users():
            if u.get("name") == name:
                return u.get("id")
        return None

    # ── preauth keys ───────────────────────────────────────────────────────
    def create_preauthkey(
        self, user: str, reusable: bool = True, ephemeral: bool = False,
        expiration: str = "24h",
    ) -> str:
        """Mint a preauth key for `user` (name). Returns the key string."""
        uid = self.user_id(user) or self.create_user(user).get("id")
        body = {
            "user": uid,
            "reusable": reusable,
            "ephemeral": ephemeral,
            "aclTags": [],
            "expiration": _rfc3339_in(expiration),
        }
        resp = self._req("POST", "/preauthkey", json=body)
        return resp.get("preAuthKey", {}).get("key", "")

    # ── nodes ──────────────────────────────────────────────────────────────
    def list_nodes(self, user: Optional[str] = None) -> list[dict]:
        q = {"user": user} if user else None
        return self._req("GET", "/node", params=q).get("nodes", [])

    def expire_node(self, node_id: str) -> dict:
        return self._req("POST", f"/node/{node_id}/expire")

    def delete_node(self, node_id: str) -> dict:
        return self._req("DELETE", f"/node/{node_id}")

    def set_routes(self, node_id: str, routes: list[str]) -> dict:
        return self._req("POST", f"/node/{node_id}/routes",
                         json={"routes": routes})


def _rfc3339_in(spec: str) -> str:
    """'24h'/'90d'/'30m' → an absolute RFC3339 expiry. Avoids Date.now in the
    library by using the local clock only here (a CLI tool, not the runtime)."""
    import datetime
    n = int("".join(c for c in spec if c.isdigit()) or "24")
    unit = "".join(c for c in spec if c.isalpha()) or "h"
    delta = {
        "m": datetime.timedelta(minutes=n),
        "h": datetime.timedelta(hours=n),
        "d": datetime.timedelta(days=n),
    }.get(unit, datetime.timedelta(hours=n))
    exp = datetime.datetime.now(datetime.timezone.utc) + delta
    return exp.strftime("%Y-%m-%dT%H:%M:%SZ")

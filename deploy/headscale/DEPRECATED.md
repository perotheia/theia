# DEPRECATED — moved to Tailscale SaaS

The fleet now uses the real Tailscale control plane (the engineer's own tailnet),
not this self-hosted Headscale. Reasons: the headscale-admin web UI doesn't
support headscale 0.29's API (all available admin image tags ship the same 0.27
app, which rejects 0.29 responses client-side), and the SaaS gives a working
admin console + a clean management API for free.

Enrollment moved to `tools/rig-enroll/` with `tailscale_client.py` (mint auth keys
via api.tailscale.com). The rig runs `tailscale up --authkey <key>` against the
DEFAULT login server — no `--login-server`. NM's vpn_observe() is unchanged
(it reads `tailscale status`, tailnet-agnostic).

This dir is kept for reference / a future self-hosted option. The container on
dalek can be torn down: `cd /opt/headscale && docker compose down`.

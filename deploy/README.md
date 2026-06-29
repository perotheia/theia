# deploy/ — local two-rig Theia bringup in Docker

Brings up two **bare-rig** containers (`central` + `compute`) that
[colony](https://github.com/perotheia/colony) provisions over SSH, driven from
the Ground Station UI — the **local mirror of a real fleet** (rpi4 + jetson),
but all-amd64 so no cross-build or physical board is needed.

The containers do **not** self-provision. They boot empty, run an `sshd` with
colony-api's pubkey in `root`'s `authorized_keys`, and wait. The full lifecycle
— enrol → provision (runtime from S3) → deploy (app via Mender / a Distribution)
— runs through the Ground Station exactly as it would against a physical rig.

## Topology

```
   host (shared network_mode: host → one network + one TIPC namespace)
  ┌─────────────────────────────────────────────────────────────┐
  │   ┌──────────────────┐         ┌──────────────────┐          │
  │   │  theia-central   │         │  theia-compute   │          │
  │   │  hostname=central│         │  hostname=compute│          │
  │   │  sshd  :2201     │         │  sshd  :2202     │          │
  │   │  (TIPC inst 0)   │ ◄─TIPC─► │  (TIPC inst 1)  │          │
  │   │  coordinator:    │         │  app compute     │          │
  │   │  supervisor+      │         │  processes       │          │
  │   │  singletons,com   │         │                  │          │
  │   └──────────────────┘         └──────────────────┘          │
  └───────┬───────────────────────────────┬─────────────────────┘
       host:2201                       host:2202
       (colony SSH → central)          (colony SSH → compute)
```

Both rigs share the host network, so their sshd ports **must** differ
(2201/2202) — a real rig owns `:22`, but two rigs on one host net can't.

## Lifecycle (colony-driven, over SSH)

1. `docker compose up` starts each rig: `sshd -D` on its port. Empty
   `/opt/theia/{bin,config}` until provisioned.
2. From the Ground Station UI → **Connect a new device**: SSH-probe the rig
   (`<host-ip>:2201` / `:2202`). colony-api's key is already authorized, so the
   probe + identity-set succeed and the rig appears as a target.
3. **Provision** (colony `orchestrate`): ansible installs the runtime/base from
   the S3 runtime plane, lays down the supervisor + executor.json, setcaps the
   binaries, configures the TIPC bearer, and starts the supervisor (systemd
   inside the container, or a foreground supervisor — colony's choice).
4. **Deploy a Distribution**: the runtime build → colony (base), the app build
   → Mender (overlay), each fanned out to the role's assigned rig.

## Quick-start

```bash
export PATH="$PWD/.venv/bin:$PATH"

# Build the rig image + bring up the two rigs.
docker compose -f deploy/docker-compose.yml build
docker compose -f deploy/docker-compose.yml up -d

# (optional) a cluster etcd, only on a host with no etcd already:
#   docker compose -f deploy/docker-compose.yml --profile etcd up -d

# Verify colony (on the GS host) can reach them:
#   ssh -p 2201 root@<host-ip> hostname   # → central
#   ssh -p 2202 root@<host-ip> hostname   # → compute

# Then drive enrol → provision → deploy from the Ground Station UI.
```

Drive TIPC directly from the host: `tdb ps` (shares the host TIPC namespace).
Tear-down: `docker compose -f deploy/docker-compose.yml down`.

## Files

| File | Role |
|---|---|
| `rig/Dockerfile`              | The bare-rig image: Ubuntu + sshd + ansible-target prereqs (python3, sudo, awscli, dpkg, libcap2-bin, tipc) + the FC runtime libs. No Theia baked in. |
| `rig/colony_authorized_keys`  | colony-api's pubkey (the provisioning key), baked into the rig's `root` authorized_keys. Matches `GET /pubkey` on colony-api. |
| `docker-compose.yml`          | The two rigs (central :2201, compute :2202) + an optional etcd profile. |
| `run-supervisor.sh`           | The local-install (non-Docker) supervisor entrypoint, exported by `BUILD.bazel` for the top-level `//:install` bundle. Not used by the rig containers. |
| `logs/` (gitignored)          | Per-rig log captures (bind-mounted to `/var/log/theia`). |

## Notes

- **All-amd64 (host abi).** Both rigs probe as `amd64`, so a Distribution role
  must carry the `amd64`/host build. To exercise the heterogeneous
  bookworm-arm64 + focal-arm64 path you need real boards (rpi4 + jetson).
- **The VUCM *sidecar* is gone.** The old self-hosted fleet mock (artifact repo
  + `campaign.sh`) was deleted — GS / Mender / colony are the fleet plane now.
  The **VUCM FC** still runs on central via the manifest; that's untouched.
- **Legacy headscale compose deleted** (VPN killed).

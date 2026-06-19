# Local Mender server — the VUCM OTA transport (minimal reusable harness)

This is the **3c** harness: a local Mender server (the fleet OTA transport) + a
thin Management-API client (`deploy/vucm/fleet.py`) that the real management server
(next release) builds on. It completes the picture from
`docs/tasks/PROGRESS/UCM-VUCM.md`:

```
fleet.py ──Mgmt API──> Mender server ──Device API──> mender client (rig)
  upload + deploy        (on dalek)        pull        → theia-release module
                                                       → ArtifactInstall_Leave
                                                       → UCM RequestUpdate (FSM)
```

The real management server will own campaigns / approval / phased rollout, but it
talks to Mender through the SAME Management API calls `fleet.py` makes — so this is
the reusable spine, not a throwaway.

## Bring up the server (on the fleet host — we use dalek)

```sh
deploy/mender/server/up.sh up                          # clone OSS mender-server @ v4.0.1 + compose up -d
deploy/mender/server/up.sh user admin@docker.mender.io password123   # initial admin
```

`up.sh` installs two theia files into the cloned server before `compose up`:
- **`docker-compose.override.yml`** — bumps traefik to v3.3 + a socket-proxy. WHY:
  on Docker ≥ 25 (dalek runs 29) the daemon enforces min Engine API 1.44; traefik
  v3.1's Docker provider negotiates at 1.24 and is rejected → **zero routes
  discovered → every /api 404s**. This was the one real bring-up blocker.
- **`theia-routes.yaml`** — static traefik file-routes mapping the API paths to the
  service containers, so routing does NOT depend on Docker-label discovery at all.
  Covers both the `/api/management/...` (fleet) and `/api/devices/...` (rig pull)
  surfaces.

The API gateway then serves `https://localhost` (self-signed). **Verified on dalek**:
login → JWT, PAT mint, and the full fleet.py upload/list path all work.

## Drive it with fleet.py (the reusable Management-API client)

```sh
# mint a PAT (up.sh token, or the UI: User → Access tokens)
export MENDER_SERVER=https://localhost
export MENDER_TOKEN=<personal-access-token>

deploy/vucm/fleet.py --insecure upload 2.0.0.mender            # upload an artifact
deploy/vucm/fleet.py --insecure artifacts                      # list them
deploy/vucm/fleet.py --insecure devices                        # enrolled devices
deploy/vucm/fleet.py --insecure deploy 2.0.0 rig-lab           # deploy to a group
deploy/vucm/fleet.py --insecure status <deployment-id>         # track rollout
deploy/vucm/fleet.py --insecure release 2.0.0 /opt/theia/releases/2.0.0 rig-lab
```

`--api-flavor oss` (default) targets the self-hosted v4 paths
(`/api/management/v1/deployments/{artifacts,deployments}`); `--api-flavor hosted`
targets hosted.mender.io's v2 layout. **Verified on dalek**: `upload` → 201 +
artifact id, `artifacts` lists them (`2.0.0 ['theia-rig']`).

## Remaining: enrol the rig (the device side)

To run the full server→rig pull, the rig's mender client must enrol to this server
and be accepted:
1. Point the rig's mender at the server (`/etc/mender/mender.conf` ServerURL =
   `https://<dalek>`, with the server CA) and `systemctl start mender-updated`.
2. Accept the device (`fleet.py devices` shows pending → accept in the UI / device-
   auth API), add it to a group.
3. `fleet.py deploy <artifact> <group>` → the rig pulls → the `theia-release`
   module lands the release-dir + symlink → `ArtifactInstall_Leave` → UCM.

The on-device half (theia-release module + state-script + UCM) is already
live-verified standalone on rig1-central (see `deploy/mender/README.md`); this step
swaps the *trigger* from `mender install <file>` to a server deployment. That's the
next-release server team's integration point — the harness here is what they drive.

## Concept map (where each piece lives)

| surface | tool | transport | when |
|---|---|---|---|
| identity / PKI / VPN | `tools/rig-enroll` | com gRPC | day-0 enrol |
| OTA upload + deploy | `deploy/vucm/fleet.py` | Mender Mgmt API | day-2 field |
| direct UCM trigger | `deploy/vucm/campaign.sh` | artheia probe | self-hosted demo |
| on-device lifecycle | `services/ucm` | (delivered release) | either path |

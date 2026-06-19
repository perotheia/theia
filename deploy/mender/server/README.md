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

## Enrol a rig — DONE for rig1-central (confirmable in the UI)

`enroll-rig.sh <rig-ssh> <server-ip>` does the whole device-side enrolment (proven
on rig1-central → the dalek server): ship the server CA + trust it, hosts entry for
the cert vhost, `mender.conf`, the identity/inventory scripts (Debian's mender-client
ships none), bootstrap, then accept the pending device via the device-auth API.

```sh
deploy/mender/server/enroll-rig.sh rig1-central 10.0.0.99
```

**Verified**: rig1-central shows in the UI as **accepted**, reporting inventory
(`hostname=raspberrypi`, `device_type=theia-rig`, `ipv4=10.0.0.22/24`,
`mac=dc:a6:32:ba:6f:e6`). Auth + inventory work over Mender's stable device API.

### Full server→rig OTA — DONE end-to-end on real hardware

A server deployment now drives the whole loop. Verified on rig1-central:

```
deployment 2.0.0 → rig → download from S3 → theia-release module:
  staged release 2.0.0 → /opt/theia/releases/2.0.0
  current → releases/2.0.0 (previous → releases/1.0.0)   ← atomic symlink switch
  committed current=releases/2.0.0
Deployment finished with status: Success
```

Rig: `current → releases/2.0.0` (`com` runs v2.0.0), `provides artifact_name=2.0.0`;
server: deployment `finished`. The complete VUCM→UCM loop: a server deployment →
Mender pull over the wire → the `theia-release` custom module → release-dir + symlink
(NOT A/B) → reported back. No `mender install <file>` — a real server-triggered field
update.

Three things were needed beyond enrolment:
1. **The 4.x client on the rig.** Debian's mender-client 3.4.0 is too old for server
   v4.0.1. `enroll-rig.sh` installs the 4.x stack via Mender's APT repo (the official
   `get.mender.io`, which publishes `+debian+trixie` arm64 packages). The 4.x client
   splits into `mender-authd` (D-Bus auth provider) + `mender-updated` (drives
   poll+install); its `artifact_name` lives in an LMDB provides store, **seeded by a
   baseline `mender-update install` of a theia-release artifact** (a blank/`unknown`
   artifact_name → the deployments endpoint 400s `cannot be blank`).
2. **Deployments route as-is** — the upstream does NOT rewrite the device-deployments
   path; the 4.x client tries v2, gets 404, falls back to v1 GET itself. (An earlier
   v2→v1 rewrite was wrong and removed.)
3. **The S3 storage route** — a high-priority `/mender` route to the seaweedfs store
   (`s3.docker.mender.io/mender/<id>` is the artifact download URL).

## Concept map (where each piece lives)

| surface | tool | transport | when |
|---|---|---|---|
| identity / PKI / VPN | `tools/rig-enroll` | com gRPC | day-0 enrol |
| OTA upload + deploy | `deploy/vucm/fleet.py` | Mender Mgmt API | day-2 field |
| direct UCM trigger | `deploy/vucm/campaign.sh` | artheia probe | self-hosted demo |
| on-device lifecycle | `services/ucm` | (delivered release) | either path |

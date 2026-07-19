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
  │   ┌──────────────────┐         ┌──────────────────┐         │
  │   │  theia-central   │         │  theia-compute   │         │
  │   │  hostname=central│         │  hostname=compute│         │
  │   │  sshd  :2201     │         │  sshd  :2202     │         │
  │   │  (TIPC inst 0)   │ ◄─TIPC─► │  (TIPC inst 1)  │         │
  │   │  coordinator:    │         │  app compute     │         │
  │   │  supervisor+     │         │  processes       │         │
  │   │  singletons,com  │         │                  │         │
  │   └──────────────────┘         └──────────────────┘         │
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
   probe + identity-set succeed and the rig appears as a target. GS matches the
   device by `identity_data.device_id` (`central`/`compute`), not MAC.
3. **Provision** (colony `orchestrate`): ansible installs the runtime/base from
   the S3 runtime plane, lays down the supervisor + executor.json, setcaps the
   binaries, configures the TIPC bearer, and starts the supervisor (systemd
   inside the container, or a foreground supervisor — colony's choice).
4. **Deploy a Distribution**: the runtime build → colony (base), the app build
   → Mender (overlay), each fanned out to the role's assigned rig.

## The update flow: VUCM ↔ UCM(s) ↔ Mender

This is the part that trips people up: **three actors, two planes, one
two-phase commit.** VUCM is the *fleet-facing orchestrator* (one per vehicle,
on the master); UCM is the *per-board installer* (one on EVERY board, master
included); Mender is the *artifact transport + fleet server* — NOT the campaign
brain. Naming: `vucm` = **V**ehicle UCM (the coordinator), `ucm` = the AUTOSAR
Update & Config Manager on each board (the `ucm*` you asked about — there are N
of them, one per board, addressed by TIPC instance = board index).

### Who talks to whom

```
  FLEET PLANE (cloud)                         VEHICLE (the rig)
  ┌───────────────┐                     ┌─────────────────────────────────────────┐
  │ Ground Station│   gRPC (VucmView)   │  MASTER (board 0)                       │
  │   operator    │ ─── CheckForCampaign ─►┌─────────┐   VucmCtlIf                │
  │   picks a     │      via com        │  │  com    │──►┌──────────┐             │
  │  Distribution │ ◄── campaign state ─│  │ (gRPC   │   │  VUCM    │ orchestr.   │
  └──────┬────────┘                     │  │  proxy) │   │ Gate+FSM │  (0x…5E)    │
         │                              │  └─────────┘   └────┬─────┘             │
         │ Mender server                │                     │ UpdateCtl         │
         │ (artifacts + deploy)         │        RequestUpdate │ .RequestUpdate   │
         ▼                              │        (fan-out to   │ /.Confirm        │
  ┌───────────────┐                     │         EACH board)  ▼   (TIPC CALL,    │
  │  Mender srv   │                     │        ┌──────────────────┐inst=board)  │
  │  artifact repo│ ◄── mender-update ──┼────────│  UCM (board 0)   │ 0x8001000E:0│
  │  + deployments│     pull artifact   │        │  UcmDaemon+FSM   │             │
  └───────────────┘        ▲            │        └────────┬─────────┘             │
                           │            │                 │ writes PROVISIONAL    │
                           │            │                 ▼ marker                │
                           │            │        ┌──────────────────┐             │
                           │            │        │ shared etcd (per)│◄──VUCM polls│
                           │            │        │ ucm_activation_* │  the barrier│
                           │            │        └──────────────────┘             │
                           │            │  ZONAL (board 1) — same UCM, inst=1     │
                           │            │        ┌──────────────────┐             │
                           └────────────┼────────│  UCM (board 1)   │ 0x8001000E:1│
                              pull      │        │  UcmDaemon+FSM   │             │
                                        │        └──────────────────┘             │
                                        └─────────────────────────────────────────┘
```

**The two planes never cross in the campaign:** Mender moves *bytes* (the
artifact), VUCM/UCM move *decisions* (the go/no-go + the two-phase commit).
GS triggers a campaign over gRPC→com→VUCM; the artifact reaches each board's
UCM over Mender independently.

### The sequence (one campaign, start to finish)

```
GS operator ──CheckForCampaign(id,version)──► com ──TIPC──► VUCM Gate
                                                              │
  1. ADMISSION   VUCM checks SM(parked) ∧ NM(link) ∧ PHM(healthy) ∧ window
                 └─ blocked → retry every 5s; admitted → ↓
  2. INSTALLING  VUCM fans UpdateCtl.RequestUpdate to EACH roster board's UCM
                 (TIPC CALL to ucm_daemon @ instance = board index):
                   UCM(0) ─┐   UCM(1) ─┐   …
                           │            │
                 each UCM: mender-update install <artifact>   ← the Mender pull
                           → holds the update PROVISIONAL (not yet active)
                           → writes ucm_activation_<board>=PROVISIONAL into etcd
  3. CONFIRMING  VUCM polls etcd every 2s (the BARRIER): counts boards whose
     (barrier)   ucm_activation_<board> is PROVISIONAL *for THIS campaign_id*.
                 └─ not all yet → keep polling (budget-bounded);
                 └─ ALL boards PROVISIONAL → ↓
  4a. OPERATOR   require_user_confirm=1 → HOLD at AWAITING_COMMIT; the operator
      (default)  calls CommitCampaign when ready → ↓
  4b. GARAGE     auto_confirm_in_window=1 AND in-window → VUCM auto-fires the
                 commit itself (pre-consent) → ↓
  5. VALIDATING  VUCM fans UpdateCtl.Confirm to EVERY board's UCM:
                 each UCM: PROVISIONAL → ACTIVE (commit), clears its marker
  6. DONE        all boards confirmed ACTIVE. (a failed/timed-out board →
                 VUCM fans Cancel → every board rolls back → ROLLBACK.)
```

Why a barrier at all: an OTA that lands unevenly (board 0 active, board 1 still
old) is a mixed-version vehicle — a safety hazard. The CONFIRMING barrier makes
activation **atomic across boards**: nobody goes ACTIVE until *everybody* is
staged (PROVISIONAL), then all commit together. That's the two-phase commit.

### Where each piece lives

| Piece | TIPC / plane | Count | Role |
|---|---|---|---|
| **VUCM** Gate + Campaign FSM | `0x8001005E` / `0x80010050`, master only | 1 per vehicle | Fleet-facing orchestrator: admission gate, cross-board barrier, two-phase commit. Reached by GS via `com`. |
| **UCM** Daemon + Gate + FSM | `0x8001000E` @ instance = board index | 1 per **board** | AUTOSAR installer: takes `RequestUpdate`, pulls+stages the artifact (Mender), holds it PROVISIONAL, activates on `Confirm`. |
| **per** (etcd) | `0x80010007`, master | 1 (shared) | The barrier's blackboard: each UCM writes `ucm_activation_<board>`; VUCM polls it. |
| **Mender** | out-of-band (cloud) | server + per-board client | Artifact repo + delivery. Moves bytes only — the campaign logic is entirely VUCM/UCM. |

> `mender-update` is the standalone install back-end
> (`services/ucm/impl/mender_install.cc`); set `THEIA_UCM_MENDER=simulate` to
> stub the pull when the artifact is pre-staged (the composer-rig default, since
> there's no live Mender server wired to these containers). The CONFIRMING
> barrier is exercised directly by seeding `ucm_activation_<board>` markers —
> see `testing/scenarios/services/vucm/vucm_barrier.robot`.

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
  The **VUCM FC** runs on the master; **UCM runs on the master AND every zonal**
  (each board installs its own OTA — see the flow above). The multi-board
  CMP_CONFIRMING barrier + garage auto-confirm are proven e2e on this rig by
  `testing/scenarios/services/vucm/vucm_barrier.robot`.
- **Enrol by identity, not MAC.** The composer containers report
  `identity_data.device_id` (`central`/`compute`), `mac=null` — GS accepts them
  by `device_id`, matching the UUID-identity design (MAC was the old model).
- **Supervisor addressing is machine-shifted.** The master's supervisor binds
  `supervisor_ctl` at `0x80020001:0`, each zonal at `:machine` (compute → `:1`),
  so PG members (which target instance 0) deterministically reach the master and
  `com`'s `for_instance(N)` reaches board N — no anycast collision in the shared
  TIPC namespace.
- **The runtime plane must not lag the manifest.** `theia release services
  --arch jammy` bakes the current `executor.json` (incl. vucm + per-board ucm)
  into the S3 plane; a stale plane deploys a supervisor tree missing FCs. If a
  board boots without vucm/ucm, re-release from HEAD then re-orchestrate.
- **Legacy headscale compose deleted** (VPN killed).

# Fleet operations from the CLI — the GS web UI as `theia` verbs

The command-line analog of the whole Ground Station (GS) web UI: **enroll a
board, list the fleet, list releases, deploy, roll out, and manage/decommission
a target** — all from the same APIs the browser drives, no browser needed. For a
headless operator on a build box or over SSH.

| GS UI | `theia` verb | Plane |
|-------|--------------|-------|
| Connect / Create-Target | `theia enroll` | onboarding |
| Fleet device table | `theia fleet` | inventory (read) |
| Releases tab | `theia releases` | S3 build catalog (read) |
| Deployment tab | `theia deploy` (+ `deploy status`) | colony + Mender |
| Rollouts tab | `theia rollout` | Mender app, phased |
| Target actions (pin/delete) + Action History clear | `theia target` | inventory + history |

The **usual order**: `enroll` a board → confirm with `fleet` → check builds with
`releases` → `deploy` (or `rollout`) → watch with `deploy status` → `target
delete` to decommission.

Both verbs are thin clients over the GS web API. They mutate the fleet
(orchestrate runtime debs, create Mender deployments), so they are **operator
verbs**, not dev-loop verbs — they live in `tools/theia.py`, NOT in the theia
MCP server (which is dev-loop + inspect only, release/deploy human-gated).

## Environment

```
$THEIA_GS_URL   GS base URL     (default http://10.0.0.99:8090)
$THEIA_GS_KEY   GS API key      (sent as X-API-Key on POST/DELETE; GET is open)
```

`$THEIA_GS_KEY` must match the gs-api `GS_API_KEY` env (lab value:
`devkey-taycann-2026`). GETs (`--list`, `status`, `list`) work without it;
anything that mutates the fleet needs it.

## The two-plane model (why deploy touches two systems)

A Theia rig runs in **two planes**, deployed by **two different systems** —
the GS aggregates both, and `theia deploy` dispatches to both in one call:

| Plane | What | Deployed by | GS authority |
|-------|------|-------------|--------------|
| **runtime / base** | the framework debs (`theia-runtime`, `theia-services`) | **colony** orchestrate — SSH, apt install, sets the device `base_version` tag | `base` |
| **app** | the application SWP (`.mender` artifact) | **Mender** — `create_deployment`, on-device `theia-swp` update module | `app` |

A **Distribution** is the named unit that binds them: `{name, version,
roles:[{role, abi, runtime_build, swp_build}]}`. Deploying a Distribution to a
device = colony(runtime_build) + Mender(swp_build), gated on the device's
inferred ABI matching the role's `abi`.

ABI is LSB-codenamed: `bookworm-arm64` (rpi4), `focal-arm64` (jetson AGX),
`jammy-arm64` (orin nano / exo), `noble-amd64` (taycann). The device's
`_rig_abi` tag must equal the role's `abi` or GS rejects the deploy.

## `theia enroll` — onboard a board (GS Connect / Create-Target)

A board must be enrolled before it can be a deploy target: GS accepts its Mender
auth-set (so Mender will deploy) and confirms it in the Observability cluster.
The board's **NAME** (the Mender `name` tag) is what every other verb resolves —
it survives re-enrol because Mender ties it to the immutable MAC.

```
theia enroll pending                        # boards checking in, not yet accepted
theia enroll connect <mac> [--name N] [--fleet F] [--group G] [--watch]
theia enroll preauth [--id UUID] --pubkey <pem|@file> [--name N] [--fleet F]
theia enroll identity <host> <uuid>         # stable identity via colony SSH
theia enroll probe <host>                   # SSH-probe MAC+hostname (prefill)
```

Two onboarding paths, matching the GS Connect modal:

- **connect (reactive)** — the board is already checking in as `pending`
  (`enroll pending` lists them by MAC). `enroll connect <mac> --name exo` accepts
  it. `--watch` polls until it shows present in the cluster.
- **preauth (proactive)** — pre-authorize BEFORE the board checks in:
  `enroll preauth --id <uuid> --pubkey @exo.pem --name exo`. Mender auto-accepts
  when the board first connects reporting that UUID identity + key. `--id`
  defaults to a fresh UUID (printed).

**Stable identity.** For a board that gets reflashed/re-imaged, pin its Mender
identity to a UUID so Mender matches it by `device_id` (not a rotating MAC):
`enroll identity <host> <uuid>` (colony SSHes in, writes the identity script,
restarts mender-auth). Pair it with `enroll preauth --id <same-uuid>` for a
stable-identity onboard that survives reflash.

## `theia fleet` — the device table (GS Fleet view)

```
theia fleet [list] [--fleet F | --group G]   # every enrolled device
theia fleet types                            # the Type dropdown options
theia fleet groups                           # named groups + counts
```

Rows show `name  fleet  base_version  reachable_ip  group [📌 if pinned]`.
`base_version` is the **live-reported** runtime (falls back to the recorded tag).
`fleet` is the hardware-capability class (Mender `device_type`); `--fleet` /
`--group` filter the list.

## `theia releases` — published builds (GS Releases tab)

```
theia releases [runtime]   # platform runtime builds, per ABI
theia releases apps        # Software Packages (fleet/app/ver + requires_runtime)
theia releases roles       # per-board role .mender bundles (L4-C campaign)
```

The `[L P]` marks: **L**ocked (deployed → immutable, re-iterate = a new version,
never a clobber) and **P**inned (operator guard against deletion). The `apps`
plane also prints each SWP's `requires_runtime` — the runtime-plane version the
deploy/rollout gate checks against (see the rollout gate below).

## `theia deploy` — one Distribution → one device

```
theia deploy <distribution> <device> [--version V] [--ip IP]
             [--publish] [--watch]
theia deploy --list
```

- `<distribution>` — a GS Distribution name (`theia deploy --list` to see them).
- `<device>` — a device **NAME** (`exo`, `taycann`) or its GS UUID. The verb
  resolves NAME → UUID via `GET /api/devices`.
- `--version V` — the Distribution version; default is the **newest** for that name.
- `--ip IP` — the IP colony SSHes to; default is the device's reachable_ip.
- `--publish` — bridge the role's app SWP `.mender` into the Mender store first
  (`POST /api/planes/apps/publish`). Needed the **first** time an app version is
  deployed; harmless to repeat. The `swp_build` string `base-0.1.0-noble-amd64`
  splits into `app=base`, `version=0.1.0-noble-amd64`.
- `--watch` — poll BOTH planes to a terminal state and print it.

The verb is **arity-1**: one role → one device (the common single-board case).
A multi-role Distribution errors out and points you at the GS UI. Extend the
verb if a multi-board CLI deploy is ever needed.

**Proven on exo:**
```
$ theia deploy exo-base exo --publish --watch
theia deploy: publish base-0.1.0-jammy-arm64 → mender: ok
theia deploy: exo-base:0.1.0 → exo
  role master: runtime 0.3.0-jammy-arm64 (colony a44f259c) + swp base-... (mender 7d009ba7)
  colony a44f259c: finished (ok)
  mender 7d009ba7: already-installed
```

`--watch` reads both planes from `GET /api/deployments` (colony `base` rows,
`status` finished/failed + `statistics.status`) and
`GET /api/deployments/{id}/devices` (Mender per-board terminal status:
success / failure / already-installed / noartifact / aborted). Returns non-zero
if either plane fails.

### `theia deploy status [device]` — the Action History

```
theia deploy status            # every deployment, both planes
theia deploy status exo        # only rows naming this device
```

Prints the aggregated `[base]`/`[app]` rows (colony + Mender), each with its
`status` and non-zero `statistics.status` counts (`success=1`,
`already-installed=1`, `failure=1`, …). The aggregate rows carry no device-id
list, so the optional `[device]` filter matches the device **name** as a
substring of the deployment name (`orchestrate-exo`, `exo-base-…`) — the same
linkage the GS Action History surfaces.

## `theia rollout` — phased app-SWP upgrade across a fleet

A rollout is a **named, stateful** entity (persisted to
`s3://theia-rollouts/<name>/index.json`): it splits the target fleet/group into
N phases, launches phase 1 now, and the operator **gates** each next phase.
**APP PLANE ONLY** — the runtime/base is (re)provisioned separately, never rolled.

```
theia rollout create <name> --app <app> --to <ver> [--fleet F | --group G]
                     [--from <ver>] [--phases N] [--scheduled]
theia rollout advance <name>     launch the next phase
theia rollout status  <name>     phases + per-board SW compare
theia rollout abort   <name>     halt (already-deployed phases stay)
theia rollout list               every named rollout
theia rollout delete  <name>     remove the entity
```

- `--app` / `--to` — the published app + target version (required for create).
- `--fleet` / `--group` — the target set; default fleet `theia-rig` (the lab fleet).
- `--from` — optional current-version filter (only devices on `from` are rolled).
- `--phases N` — split into N phases (default 2).
- `--scheduled` — create WITHOUT launching phase 1 (gate it manually with `advance`).

`create` hits the GS **runtime-compat gate**: every targeted device's
`base_version` must satisfy the app SWP's `requires_runtime`. The gate compares
the base_version **semver prefix** (`0.3.0-jammy-arm64` → `0.3.0`) against the
bare requirement — a device on the wrong runtime blocks the rollout with a 409
`runtime-incompatible`.  (This is APP-plane; provision the runtime plane first
with `theia deploy` if the gate rejects.)

`status`/`list` read `phases` as a **list of phase dicts** and `current_phase`
as the index — not counts. `status` also prints `sw_compare`: per-board
current→target with a ✓ when a board is already at target.

**Proven on exo (full lifecycle, 3-device `theia-rig` fleet, 2 phases):**
`create` → phase 1 launched (Mender deploying) → `status` (phase 1/2, sw_compare) →
`advance` → `abort` → `delete`.

## `theia target` — manage / decommission a device (GS Target actions)

```
theia target pin    <device>     # guard from deletion
theia target unpin  <device>
theia target delete <device>     # decommission (Mender delete) — GUARDED
theia target clear  [device]     # clear FINISHED deploy history (Action History)
```

`delete` is the **inverse of enroll** — the board leaves the fleet. It is
GUARDED: a pinned device must be `unpin`'d first (the destructive op is
deliberately two-step). `clear` prunes finished colony actions from the shared
Action History — no device = a GLOBAL prune; a device scopes the colony prune to
that rig. `<device>` is a NAME or GS id.

> Note: pin/unpin write a Mender `pinned` tag; the tag write is accepted
> immediately but Mender inventory can lag reflecting a tag *deletion* — a
> just-unpinned device may still read `pinned` for a short window (a GS/Mender
> propagation characteristic, not a CLI defect).

## GS route map (what each verb calls)

| Verb | Method + route |
|------|----------------|
| `enroll pending` | `GET /api/devices/pending` |
| `enroll connect` | `POST /api/devices/connect` |
| `enroll preauth` | `POST /api/devices/preauthorize` |
| `enroll identity` | `POST /api/devices/set-identity` |
| `enroll probe` | `GET /api/devices/probe?host=` |
| `fleet` / `fleet types` / `fleet groups` | `GET /api/devices`, `/api/devices/types`, `/api/devices/groups/list` |
| `releases runtime\|apps\|roles` | `GET /api/planes/runtime\|apps\|roles` |
| `deploy --list` | `GET /api/planes/distributions` |
| `deploy` (resolve device) | `GET /api/devices` |
| `deploy --publish` | `POST /api/planes/apps/publish` |
| `deploy` | `POST /api/deployments/distribution` |
| `deploy status` / `--watch` | `GET /api/deployments`, `GET /api/deployments/{id}/devices` |
| `target pin\|unpin` | `POST /api/devices/{id}/pin` |
| `target delete` | `DELETE /api/devices/{id}` |
| `target clear` | `POST /api/deployments/clear` |
| `rollout create` | `POST /api/deployments/rollouts` |
| `rollout advance` | `POST /api/deployments/rollouts/advance` |
| `rollout status` | `GET /api/deployments/rollouts/{name}` |
| `rollout abort` | `POST /api/deployments/rollouts/{name}/abort` |
| `rollout list` | `GET /api/deployments/rollouts` |
| `rollout delete` | `DELETE /api/deployments/rollouts/{name}` |

## Mental model

- `theia enroll` = **admit a board** to the fleet (Mender accept + cluster confirm).
- `theia fleet` / `theia releases` = **read** the inventory and the build catalog.
- `theia deploy` = **install a specific version** on a specific board (both planes, once).
- `theia rollout` = **migrate a fleet's app** version-to-version, phased and gated.
- `theia deploy status` = **read** the Action History (what happened, both planes).
- `theia target` = **guard / decommission** a board, or **clear** the history.
- Both defer all policy to the GS: ABI gate, runtime-compat gate, colony/Mender
  orchestration. The verbs just marshal the request and report the terminal state —
  they never bypass the GS. If a deploy is rejected, the GS said no; fix the
  device tags / runtime plane, don't work around it.

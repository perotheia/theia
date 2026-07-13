# Fleet deploy + rollout from the CLI — `theia deploy` / `theia rollout`

The command-line analog of the Ground Station (GS) web UI's **Deployment** and
**Rollouts** tabs. Same two APIs the browser drives, no browser needed — for a
headless operator on a build box or over SSH.

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

## GS route map (what each verb calls)

| Verb | Method + route |
|------|----------------|
| `deploy --list` | `GET /api/planes/distributions` |
| `deploy` (resolve device) | `GET /api/devices` |
| `deploy --publish` | `POST /api/planes/apps/publish` |
| `deploy` | `POST /api/deployments/distribution` |
| `deploy --watch` | `GET /api/deployments`, `GET /api/deployments/{id}/devices` |
| `rollout create` | `POST /api/deployments/rollouts` |
| `rollout advance` | `POST /api/deployments/rollouts/advance` |
| `rollout status` | `GET /api/deployments/rollouts/{name}` |
| `rollout abort` | `POST /api/deployments/rollouts/{name}/abort` |
| `rollout list` | `GET /api/deployments/rollouts` |
| `rollout delete` | `DELETE /api/deployments/rollouts/{name}` |

## Mental model

- `theia deploy` = **install a specific version** on a specific board (both planes, once).
- `theia rollout` = **migrate a fleet's app** version-to-version, phased and gated.
- Both defer all policy to the GS: ABI gate, runtime-compat gate, colony/Mender
  orchestration. The verbs just marshal the request and report the terminal state —
  they never bypass the GS. If a deploy is rejected, the GS said no; fix the
  device tags / runtime plane, don't work around it.

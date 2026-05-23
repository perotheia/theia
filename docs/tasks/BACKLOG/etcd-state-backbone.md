# etcd state backbone + services/db proxy

**Goal:** make etcd the single source of truth for live Theia state.
Supervisor publishes ChildState / SupervisionEvent / tombstone
metadata into etcd. The GUI reads etcd directly for keys-and-values
browsing (observer's ETS-tab analogue). Applications go through a
new `services/db` — a **state + OTA controller** that owns schema
versioning *and* manages safe A/B updates with health gating and
revision/snapshot rollback. etcd's CAS + revision history + snapshot
primitives mean we don't need SQL-style migrations: lazy schema
adaptation on read is the default; bulk rewrite is the fallback for
non-trivial reshuffles. See Phase 6 for the full OTA model.

## Topology

```
  ┌─────────────────────────┐
  │  supervisor-gui  (wx)   │
  └────┬───────────────┬────┘
       │ gRPC          │ etcd v3 (gRPC)
       │ (state-shape) │ (raw KV browse)
       ▼               │
  ┌─────────┐          │
  │ svc/db  │          │
  └────┬────┘          │
       │ etcd v3       │
       ▼               ▼
  ┌────────────────────────┐
  │  etcd  (host daemon)   │  listen 127.0.0.1:2379
  └─────────▲──────────────┘
            │ etcd v3 writes
  ┌─────────┴──────────────┐
  │  supervisor  (per host)│
  └────────────────────────┘
```

Two readers, one writer:

- **etcd writes** → only supervisor (and services/db on behalf of
  applications). GUI never writes to etcd directly.
- **etcd reads** → GUI (Table Viewer + Watch for live keys), services/db
  (state-shape reads with schema awareness).
- **services/db** → C++ service. Owns the application state schema,
  handles forward/backward migrations across schema versions,
  exposes typed RPCs to applications. Think gRPC + Protocol Buffers
  on the front, etcd v3 on the back.

## Why now

- supdbg validates we can drive the supervisor remotely; etcd lets
  us reason about cluster state across machines without each panel
  re-implementing its own gRPC subscriber.
- The observer port (`docs/tasks/BACKLOG/extend-supervisor-GUI.md`)
  needs a Table Viewer tab; this is the right cut-over point — the
  Tabs that follow (System, Load Charts, etc.) read etcd by key
  prefix instead of buffering gRPC streams in panel-local state.
- Migration story for application state needs to land before any
  FC writes persistent data; services/db gives us a place to put
  it.

## Key layout (proposed)

```
/theia/
├─ machines/                       # one key per host
│  ├─ central                      # value: machines.yaml-ish JSON
│  └─ compute
├─ state/                          # live, supervisor-managed
│  ├─ central/
│  │  ├─ tree/<gen>                # latest TreeSnapshot, TTL
│  │  ├─ health                    # latest HealthBeacon, TTL
│  │  └─ child/<name>              # latest ChildState
│  └─ compute/...
├─ events/                         # SupervisionEvent ring
│  ├─ central/<ts>-<seq>           # value: serialized SupervisionEvent
│  └─ compute/<ts>-<seq>           # capped via TTL or compaction
├─ tombstones/                     # post-mortem index
│  ├─ central/<child>/<ts>         # value: tombstone manifest
│  └─ ...
└─ app/                            # services/db-managed
   ├─ schema/<app>/version         # current schema version
   └─ data/<app>/<schema_v>/...    # typed app state
```

GUI tabs map to prefixes:

| Tab            | Prefix                          | Operation |
|---|---|---|
| System         | `/theia/state/<m>/health`       | Get + Watch |
| Load Charts    | `/theia/state/<m>/health`       | Watch (rolling buffer in GUI) |
| Memory         | `/theia/state/<m>/child/`       | RangeGet + Watch |
| Applications   | `/theia/state/<m>/tree/`        | Get (latest gen) + Watch |
| Processes      | `/theia/state/<m>/child/`       | RangeGet + Watch |
| Sockets        | `/theia/state/<m>/child/*/sockets` (TBD pre-work) |
| Table Viewer   | user-typed prefix               | RangeGet + Watch (etcd browse) |
| Tombstones     | `/theia/tombstones/`            | RangeGet + Watch |
| Trace Overview | `/theia/events/`                | Watch + bounded history |

## Phases

### Phase 1 — etcd on the host
- `sudo apt install etcd-server etcd-client` (Ubuntu 22.04 ships
  etcd 3.4; for newer we use the upstream tarball).
- Configure as a single-node localhost daemon: `--listen-client-urls
  http://127.0.0.1:2379 --advertise-client-urls http://127.0.0.1:2379
  --data-dir /var/lib/theia-etcd`. systemd unit lives at
  `/etc/systemd/system/theia-etcd.service` (NOT the apt-shipped
  `etcd.service` since that name conflicts).
- Smoke: `etcdctl put /theia/hello world && etcdctl get /theia/hello`.
- Docker compose containers reach the host etcd via
  `host.docker.internal:2379` (already enabled on Ubuntu via
  `extra_hosts: ["host.docker.internal:host-gateway"]`).

### Phase 2 — supervisor writes ChildState + SupervisionEvent
- Add etcd v3 client to platform/supervisor (etcd-cpp-apiv3 via
  CMake `find_package` if available; otherwise vendor a minimal
  gRPC client built off the etcd .proto).
- Periodic publisher (1Hz): serialize TreeSnapshot, HealthBeacon,
  per-child ChildState; PUT to `/theia/state/<machine>/...` with a
  lease + TTL so stale data falls off when supervisor dies.
- Event-driven: every SupervisionEvent → PUT
  `/theia/events/<machine>/<ts>-<seq>`.
- Toggle via executor.yaml: `etcd_endpoints: ["127.0.0.1:2379"]`,
  default off so single-machine runs without etcd still work.

### Phase 3 — supdbg etcd backend
- supdbg gets `--backend etcd` flag. Same commands (tree, watch,
  wait, restart) but talks to etcd Watch streams instead of
  services/com Subscribe. Validates the schema before any GUI work
  consumes it.
- Tester loop: `supdbg --backend etcd watch` ≡ today's
  `supdbg watch` against services/com.

### Phase 4 — Table Viewer tab in supervisor-gui (raw etcd browse)
- New panel reads etcd v3 directly via etcd-cpp-apiv3 (gRPC).
- Prefix filter input at top; key list below; click a key shows
  value in a side pane (text/JSON/hex auto-detect).
- Watch mode toggles live updates.
- This is the observer Table Viewer analogue, modulo ETS → etcd.

### Phase 5 — Tabs migrate to etcd reads
- System, Load Charts, Memory, Processes, Applications, Tombstones,
  Trace Overview each move to `etcd Watch` on their assigned prefix
  instead of subscribing to services/com.
- services/com stays alive for control RPCs (restart/terminate/etc).
- One commit per tab so the tester can iterate.

### Phase 6 — services/db = state + OTA controller
**Re-scoped (was "DB migration service").** etcd already gives us
versioned KV, CAS transactions, snapshots, and revision history —
so traditional SQL-style migrations are overkill. Instead
`services/db` is the **state-shape gatekeeper + OTA controller**:
it owns the active-version pointer, runs health gating before
promotions, and exposes snapshot/rollback primitives.

#### State-shape API (typed reads / writes)
- `Put(app, schema_v, key, value)` — value is a typed proto.
  Writes to `/theia/app/data/<app>/v<schema_v>/<key>`.
- `Get(app, schema_v, key)` — returns the typed value. If only an
  older `v<n>` exists, performs **lazy migration** on the read path
  (Option A from the design discussion): the app declares "give me v2",
  services/db detects v1 in etcd, runs the in-memory v1→v2 transform,
  returns v2. Default for low-friction OTA.
- `Watch(app, schema_v, prefix)` — streamed, post-migration.
- `RegisterSchema(app, schema_v, .proto_digest)` — declares a
  schema version; stored at `/theia/app/schema/<app>/v<n>`.
- Migration transforms live in services/db source tree as
  `migrations/<app>/v<from>_to_v<to>.cc` — compiled into the
  binary. Reuse for both lazy-read and bulk rewrite paths.

#### OTA controller API (slot lifecycle)
Software A/B (no hardware partitions on Pi 4) — emulated via two
filesystem slots + an etcd pointer:

```
/opt/theia/slots/A/    # binaries + executor.yaml for version A
/opt/theia/slots/B/    # binaries for version B (staging)
/opt/theia/current     # symlink → slots/<A|B>
```

etcd carries the matching pointer:
```
/theia/ota/active_slot   = "A"
/theia/ota/staging_slot  = "B"        # may be empty
/theia/ota/active_version = "1.4.2"
```

RPCs:
- `StageUpdate(bundle_url, schema_v, dest_slot)` — supervisor fetches
  the bundle into the inactive slot. App + config + schema_v travel
  together. Verifies signature (sigstore-style detached sig, TBD).
- `RunHealthGate(slot)` — boots the staged slot in a sandboxed
  supervisor sub-tree, runs the watchdog checks (CPU sane, IPC
  functional, etcd reachable, timing budgets met). Returns
  ControlReply with verdict.
- `Promote(slot)` — atomically: snapshot etcd, flip
  `/opt/theia/current`, switch `/theia/ota/active_slot`,
  reload supervisor's executor.yaml.
- `Rollback(level)` — three levels per the design doc:
  - **L1 instant**: flip the pointer back to the previous slot.
    No etcd restore; whatever state changes the new version made
    stay (apps see them via lazy-read).
  - **L2 revision**: `etcdctl move-revision <pre_promote>`;
    revives the pre-promotion KV view.
  - **L3 snapshot**: `etcdctl snapshot restore <pre_ota.db>`;
    full datastore rewind. Reserved for "the new version corrupted
    state and L1/L2 didn't catch it."
- `Snapshot(label)` — `etcdctl snapshot save`. Tagged in
  `/theia/ota/snapshots/<label>`.

#### Bulk rewrite (Option B from discussion, fallback)
- `MigrateBulk(app, from_v, to_v)` — services/db reads every
  `/theia/app/data/<app>/v<from>/...`, runs the transform, writes
  to `v<to>`. Lease-protected so a crash mid-migration leaves a
  resumable marker at `/theia/app/migrations/<app>/<from>_<to>`.
- Useful when v→v+1 stops being expressible lazily (e.g. key
  reshuffle, not just value reshape).

#### Watchdog + health gating
- Reads supervisor's HealthBeacon stream (already published to
  `/theia/state/<m>/health` in Phase 2).
- Plus app-specific liveness keys at
  `/theia/state/<m>/child/<name>/heartbeat` (also Phase 2).
- Promotion blocks until all watchdog conditions hold for N
  consecutive seconds (configurable per app).

#### What it deliberately doesn't do (yet)
- Update bundle signing — flagged as critical-but-out-of-scope here;
  separate BACKLOG entry. Phase 6 accepts unsigned bundles with a
  big warning log.
- Multi-machine coordinated rollout (canary % of fleet, etc.) —
  single-machine staged + promoted for now.
- Bundle distribution (HTTPS endpoint, mirror, content addressing) —
  Phase 6 accepts a file:// or local path; the network layer is
  separate work.

#### Smoke / definition of done
1. `services/db` running locally; `RegisterSchema(myapp, 1, …)` +
   `Put + Get` round-trip.
2. Stage a fake v2 of `myapp` into slot B; `RunHealthGate` passes;
   `Promote` flips `/opt/theia/current`; supervisor reloads; apps
   come up on v2.
3. `Rollback(L1)` flips back instantly; supervisor reloads onto A.
4. Lazy migration: write a v1 key, read with schema_v=2, get back
   a v2 value with the transform applied.
5. Bulk migration: 1000 keys at v1 → MigrateBulk → 1000 keys at v2,
   resumable across kill -9.

### Phase 7 — Docs + tester handoff
- `docs/etcd.md` covering layout, lease/TTL semantics, GUI flow,
  services/db schema model.
- `docs/skills/theia-state/SKILL.md` (or similar) so future agents
  know to write to etcd, not invent their own pub/sub.
- Tester runs the supervisor-gui against the docker compose stack
  and watches keys appear / disappear as services restart.

## Out of scope

- Multi-node etcd cluster — single-node localhost is enough for dev
  + first customer board. Real HA comes later.
- Backups / snapshot rotation — out of scope until customer deploy.
- TLS — bind to 127.0.0.1 only; no auth on the wire today.

## Pre-existing dependencies / blocks

- The observer-port BACKLOG (`extend-supervisor-GUI.md`) now reads
  this as its data source. The phase 1+2 work here unblocks phases
  2-9 of that plan.
- services/com gRPC subscribe stream is NOT removed by this work;
  it's still the control plane. supdbg keeps working unchanged.

## What "no stubs" means here

- Phase 1 done = `etcdctl get /theia/hello` works from host AND
  from inside both compose containers.
- Phase 2 done = `etcdctl get --prefix /theia/state/` shows live
  TreeSnapshot / HealthBeacon while the supervisor is running.
- Phase 3 done = `supdbg --backend etcd tree` returns the same data
  as `supdbg tree`.
- Phase 4 done = GUI Table Viewer browses every key in real-time.
- Phase 5 done = at least one observer-port tab reads exclusively
  from etcd, services/com unused on that tab's path.
- Phase 6 done = the example app round-trips through services/db
  with a migration.

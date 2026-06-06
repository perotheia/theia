# services/db — config & state gatekeeper (sole etcd connection)

> **DONE (2026-06-06), implemented on `services/per`** (AUTOSAR Persistency —
> the natural home). All steps landed + probe-verified:
> - S1 `.art` (PerClientIf + PerManagerIf, proto gen-app'd) ✓
> - S2 params per-FC JSON + runtime config singleton ✓
> - S3 `const` param + config-schema registry (shipped as `gen-schema` +
>   process-global SchemaRegistry) ✓
> - S4 ConfigUpdated framework cast + Get/Put/Watch ✓
> - S5 lazy migration-on-read (digest-keyed BFS chain) ✓
> - S6 MigrateBulk via dlopen plugin + keyspace walk + CAS ✓
>   **— EXCEPT the lease-bound crash-safe RESUME marker (deferred).** A bulk
>   migrate that's killed mid-flight restarts from scratch, not from where it
>   died. Covered by the e2e-test backlog item.
> - S7 Snapshot/RestoreSnapshot (config-prefix scoped) ✓
> - S8 wired into rig/manifest (`stage-local` emits `config/<fc>.json`; per in
>   the executor tree) ✓
> - S9 example app round-trips (9 probe e2e tests) ✓
>
> Design evolved past this doc in two ways (user decisions): the migration
> RUNTIME is nanopb-struct, not JSON (JSON is the design-tool-only layer in
> migrate.py); a full migration tooling chain (gen-schema / tdb get-snapshot /
> migrate.py / gen-transform) was added — see docs/artheia/transform.md +
> docs/skills/theia/references/migration.md.

Aggregates: task #239 (TaskList) + Phase 6 of
[`BACKLOG/etcd-state-backbone.md`](../BACKLOG/etcd-state-backbone.md).
Supersedes the earlier "schema-version on the etcd path" design — see
**Redesign note** below.

## Goal

Stand up `services/db` as the **single process that talks to etcd** and the
typed gatekeeper for two distinct kinds of node-owned values:

- **params** — static deployment knobs (scalars, set pre-install, read once).
  Delivered as a generated per-FC JSON file read by a runtime config singleton —
  NOT through etcd/services-db.
- **config** — structured, runtime-observable configuration (protobuf, hot-
  reloadable, delivered as immutable snapshots with a change mask). This is the
  part that goes through services/db.

Apps NEVER connect to etcd and NEVER link the etcd C++ client. Only `services/db`
holds the etcd connection; every other node reaches state through it (via
`services/com` / `SupervisorCtl` for the control path, never raw etcd). This
keeps the etcd-cpp-apiv3 dependency, its CMake/Bazel link, and the
connection-pool lifecycle in exactly one place, and lets the backing store be
swapped (etcd → json → sqlite → consul) without touching a single node.

The boundary stays sharp: **services/db owns data shape + the store
connection; Puppet owns deploy.** OTA / A-B slots / health gates are out of
scope.

## Redesign note (why this doc changed)

The previous version keyed everything on a **schema version embedded in the etcd
path** (`/theia/app/data/<app>/v<v>/<key>`) and made every reshape a path-level
migration. Two things forced a rethink:

1. **etcd was dropped from the supervisor** in the gen-app migration
   (`runtime.h`: "etcd dropped — all-internal-TIPC"; `EtcdPublisher` removed).
   So "etcd backbone live" is no longer true — services/db now *reintroduces*
   etcd behind a single proxy, rather than the old every-component-connects
   model. That is the whole point of the service.
2. The right axis is **lifecycle, not storage format or path version**. A value
   is classified by *when it changes, who changes it, and how the app must
   react* — not by whether it's a scalar or a nested protobuf. Versioning is a
   property of the **value/schema**, not of the **key path**: the path is
   stable; `#version` is metadata services/db strips before it forwards.

## The two value kinds (lifecycle model)

The `.art` grammar already carries both on a node:

```art
node atomic SupervisorCtl {
    params {                          // STATIC deployment knobs
        heartbeat_period_ms : uint32 = 1000
        snapshot_period_ms  : uint32 = 5000
        watchdog_max_missed : uint32 = 3
    }
    config SupervisorConfig           // structured runtime config (a MessageDecl)
}
```

### params — static, read-at-startup (generated JSON + runtime config singleton)

```text
read at startup · rarely changed · simple scalars · change ⇒ restart
```

params do NOT go through etcd or services/db. They are pure static deployment
configuration, delivered as a **generated per-FC JSON file** that the runtime
reads once at process boot into a **config singleton**:

- **One JSON per FC, sections per node.** `gen-app` (param emitter) walks the
  FC's nodes and emits a single config file keyed by **node name**:

  ```json
  // /ROOT/<machine>/config/<fc>.json
  {
    "supervisor_ctl": {
      "heartbeat_period_ms": 1000,
      "snapshot_period_ms":  5000,
      "watchdog_max_missed": 3
    },
    "supervisor_worker": { ... }
  }
  ```

  Path: `/ROOT/<machine>/config/<fc>.json` — staged next to the binaries the
  same way `executor.json` / `netgraph.json` are. (Reuses the
  `generate_etcd_schema` param walk, but the OUTPUT is this per-FC JSON, not an
  etcd seed.) System engineers edit the JSON pre-install; this IS the
  deployment configuration.

- **Runtime config reader + singleton.** The runtime gains a config reader; the
  generated `main.cc` constructs a process-wide config singleton early in boot,
  pointed at `$THEIA_ROOT_DIR/<machine>/config/<fc>.json` (the supervisor
  already knows `root_dir_` + `machine_name` and sets the child env; `<fc>` is
  the app/component name). It parses the JSON once.

- **Nodes pull values via `runtime->get_config()`** in their ctor or `init()`:

  ```cpp
  // in NodeXState ctor / init()
  auto cfg = ::theia::runtime::get_config();            // process singleton
  period_ms_ = cfg.node(kNodeName).u32("heartbeat_period_ms", 1000);
  ```

  Read-once: params are static, so the node caches what it needs at construction
  and a change requires a restart (the singleton is not re-read live).

- **`const` (read-only) params** — a `const` modifier on `NodeParam` for values
  the app must never mutate (e.g. a wire id). gen-app emits them as `const`
  members; with the JSON model there is no writer anyway, but `const` documents
  intent and lets a future writer reject the field.

This keeps params self-contained per FC, with zero etcd/services-db dependency —
exactly the static-deploy lifecycle. Only **config** (below) goes through
services/db.

### config — structured, runtime-observable

```text
hierarchical · runtime-managed · possibly hot-updated · versioned · observable
```

- A protobuf `MessageDecl` (the node's `config <Msg>`). Stored in etcd as
  **binary protobuf** (the serialized message bytes) — NOT JSON. The node
  unpacks it client-side (`ParseFromString`/nanopb decode). Versioned by
  **schema digest**, not by a path segment. (Storing the wire bytes keeps the
  value compact and avoids a JSON encode/decode hop; a JSON view, if ever needed
  for a web UI / REST, is a render concern derived from the proto on demand, not
  the stored form.)

- Delivered as a **cast the runtime handles** (see Architecture): an immutable
  snapshot + a change mask; the framework swaps a `shared_ptr<const Config>`,
  never mutates in place. The carrier message is a **framework type** in the
  runtime proto (sibling of `TraceControlPush`/`LogLevelPush`), carrying the
  config as opaque bytes + its digest so it's node-type-agnostic:

  ```proto
  // platform/runtime proto — generic config-service push (framework-handled)
  message ConfigUpdated {
    bytes               config  = 1;   // serialized <Node>Config; node unpacks
    string              digest  = 2;   // schema the bytes are encoded under
    google.protobuf.FieldMask changed = 3;   // exactly what changed
  }
  ```

  The node's framework `handle_cast(ConfigUpdated)` decodes the bytes to its
  `config` type and exposes the typed snapshot + mask. Apps know precisely what
  changed without diffing the whole tree.

#### Who computes the FieldMask — services/db (decided)

**services/db computes the mask. etcd cannot.** Resolution of the two options:

- **Option 2 — etcd plugin computes it: REJECTED, not realistic.** etcd is
  schema-blind: a stored value is opaque protobuf bytes to it. etcd's watch
  *does* carry the previous value (`etcd::Event::has_prev_kv()`/`prev_kv()` in
  the vendored etcd-cpp-apiv3 when the watcher is created `WithPrevKV`), but
  that's a raw prev-vs-new byte pair — etcd has no way to deserialize the proto
  or produce a `google.protobuf.FieldMask` over its fields. A typed field diff
  needs the schema; only services/db has it (the digest→proto registry).

- **Option 1 — services/db computes it: CHOSEN.** On a watch event services/db:
  1. parses `prev_kv` (old) and `kv` (new) as the node's proto, via the schema
     registry digest;
  2. computes the typed diff → `FieldMask changed`;
  3. casts `ConfigUpdated{config=new, changed=mask}` to the owning node.

  **No snapshot cache is required for correctness** — etcd's `prev_kv` already
  supplies the "before" side, so services/db diffs the two values the watch hands
  it. (A snapshot cache is purely an optimization, see below.)

- **Lazy snapshot (extension, optional optimization).** services/db keeps a
  per-FC cached `(digest, parsed-config)` entry **only after that FC first asks
  for its config** (`GetConfig`/`WatchConfig`). Then it can: serve `GetConfig`
  without an etcd round-trip, and diff against the cache instead of re-parsing
  `prev_kv` each event. An FC that never asks for config gets no cache entry.
  This is a latency/alloc optimization layered on Option 1 — not needed for the
  mask to be correct.

### Per-field dynamic-update classification

Not every field can be hot-reloaded safely. Each config field is tagged (proto
field option or an `.art` annotation) with one of:

```text
STATIC          — set once, never changes after start
HOT_RELOAD      — applied live on ConfigUpdated
RESTART_REQUIRED — change recorded, takes effect on next start
```

Example:

```proto
message SupervisorConfig {
  TracePolicy   trace   = 1;   // HOT_RELOAD
  LoggingConfig logging = 2;   // HOT_RELOAD
  ClusterConfig cluster = 3;   // RESTART_REQUIRED
}
```

The receiving node's framework config handler refuses to hot-apply a
RESTART_REQUIRED field (it consults the field's class) and instead marks it
pending-restart, so we never try to live-mutate something that can't change
safely. (services/db sends the full snapshot + mask; the node decides what it
may apply live.)

## Architecture — db svc is the only etcd client

```text
   app node                                   services/db
  ┌──────────────────────────┐            ┌──────────────────────┐
  │ framework handle_cast:    │  cast      │ DbClient (client API)│
  │   TraceControlPush  ──────┼◄───────────┤   Get/Put/Watch      │
  │   LogLevelPush      ──────┤  Config    │ DbManager (mgr API)  │
  │   ConfigUpdated  ◄────────┤  Updated   │   schema/snap/migrate│
  │  (cfg, FieldMask) hot-    │  (cast)    └─────────┬────────────┘
  │   applies; on_config_     │                      │ SOLE etcd conn
  │   update(cfg, mask) hook  │            etcd ◄─────┘ (WithPrevKV watch)
  └──────────────────────────┘
```

- **ConfigUpdated is a CAST handled by the RUNTIME, not the app.** The runtime
  already routes two supervisor config-service casts into a node's framework
  `handle_cast` (GenServer.hh): `LogLevelPush` (#386) and `TraceControlPush`
  (#403). `ConfigUpdated` is the **third** of the same kind: services/db casts
  it to the node's config-service receiver (the TIPC name every reporting node
  already binds in main.cc), the framework `handle_cast` swaps the
  `shared_ptr<const Config>` and honors the per-field STATIC/HOT_RELOAD/
  RESTART_REQUIRED class. The app only overrides `on_config_update(cfg, mask)`
  if it wants the hook; it never writes a watch loop, never sees etcd.
- **Who casts it.** services/db casts ConfigUpdated DIRECTLY to the target node
  (it holds the netgraph addr like any sender) — config is durable in etcd, so
  unlike trace/log it does NOT need to route through the supervisor for
  restart-survival: a restarted node re-reads via `GetConfig` on boot and
  re-arms `WatchConfig`. (The supervisor stays out of the config data path.)
- **`#version` stripping:** the stored value carries a schema-digest tag;
  services/db migrates to the schema the *requesting node* speaks, strips the
  version metadata, casts a clean typed snapshot. Path stays
  `/theia/config/<node>` — stable, version-free.
- **Proxy writes:** a node's `Put` (DbClientIf) goes to services/db, which
  writes etcd. No node links etcd; connection, retries, lease/watch lifecycle
  live only in services/db. Swapping the backend touches services/db alone.

## Versioning & migration (redesigned)

Versioning attaches to the **schema/value**, not the path.

- `services/db` keeps a schema registry keyed by **proto digest** per node-config
  type (from `artheia gen-db`, see below). A stored value records the digest it
  was written under.
- **On read:** if the stored digest ≠ the requesting node's digest, services/db
  runs the registered transform chain (`vN→vN+1→…`) to the requester's schema,
  strips the version tag, returns the migrated snapshot. Lazy, transparent.
- **On write to an older schema:** services/db maps the old-schema value forward
  to the current schema before persisting (so the store converges; readers at
  the new schema don't pay the transform).
- **Bulk reshape** (`MigrateBulk`) for changes that don't fit the lazy path:
  rewrite every value under a node's config prefix through the transform chain,
  crash-safe resumable via a lease-bound marker.
- Transforms live in `services/db/migrations/<node>/<from>_to_<to>.cc`,
  static-init registrar pattern (same shape as
  `platform/runtime/trace/trace_decoder_protos.cc`).

## Generators — params JSON vs config schema

Two distinct, independent generation paths (matching the two lifecycles):

- **params → per-FC JSON** (`gen-app` param emitter). Walks each FC's nodes,
  emits `/ROOT/<machine>/config/<fc>.json` with one section per node name
  (reuses the `generate_etcd_schema` param walk, but writes per-FC JSON instead
  of an etcd seed). No services/db involvement. `const` params flagged.
- **config → schema registry** (`artheia gen-db`). Walks all packages, records
  the proto digest per node-`config` type for the services/db schema registry +
  migration chains. Config only — params never touch services/db.

## Scope (landable steps)

### Step 1 — `.art` declaration (proto is GENERATED, not hand-written)

**IMPLEMENTED ON services/per** (AUTOSAR Persistency — the natural home; per was
already scaffolded as the etcd proxy). The real `.art` lives in
`services/per/system/per/{package.art, component.art}`; gen-app emits the proto +
lib/main scaffolding (`gen-app --kind fc`). There is **no hand-written
`db.proto`** — the messages and the service surface come from the `.art`
interfaces.

The surface splits into **two interfaces / two nodes**:

- **PerClientIf** (PerClient, tipc 0x80010007) — `GetConfig` / `PutConfig` /
  `WatchConfig`. Hot path, every app.
- **PerManagerIf** (PerManager, tipc 0x80010008) — schema registry, bulk
  migrate, snapshot/restore. Ops-only, separate port so it can be ACL'd /
  firewalled independently and never collides with the client hot path.

**WatchConfig is a SUBSCRIPTION — how the update gets back to the client.**
`WatchConfig` returns a plain ack (`PerReply`); it only ARMS the watch. The
update is delivered as a `platform.runtime.ConfigUpdated` CAST to the
subscriber's framework config-service receiver — the THIRD config-service push,
added to the runtime's `ChildControlIf` (`data config_push`) alongside
trace_ctrl/log_level. Every gen-app node already binds that receiver and
register_casts the control pushes, so a subscriber receives `ConfigUpdated` for
free (one more `register_cast`). The SENDER side is modeled on `PerClient` as
`sender config_out provides ChildControlIf` (mirrors the supervisor's `sender
child_ctrl provides ChildControlIf`); the address is dynamic — resolved from
`WatchConfigReq.subscriber_node` at run time via the cast-to-runtime-addr
override, not a static netgraph peer. So: client calls `WatchConfig` → per
records (subscriber_node, target_node) → on an etcd change per casts
`ConfigUpdated` to subscriber_node → that node's framework `handle_cast` applies
it. The client never writes a watch loop or a streaming RPC.

```art
package system.services.db

// ── Client API: config Get/Put/Watch (every app) ──────────────────
interface clientServer DbClientIf {
    operation GetConfig(in r: GetConfigReq)   returns ConfigSnapshot  // bytes; #version stripped, migrated to caller schema
    operation PutConfig(in r: PutConfigReq)   returns ControlReply    // maps-forward to current schema
    operation WatchConfig(in r: WatchConfigReq) returns ControlReply  // arms a watch; updates DELIVERED as a cast (see below)
}

// ── Manager API: schema + snapshot + bulk (ops/tooling only) ──────
interface clientServer DbManagerIf {
    operation RegisterSchema(in r: RegisterSchemaReq) returns ControlReply
    operation ListSchemas(in r: ListSchemasReq)       returns SchemaList
    operation MigrateBulk(in r: MigrateBulkReq)       returns ControlReply  // progress via events/log
    operation Snapshot(in r: SnapshotReq)             returns ControlReply
    operation RestoreSnapshot(in r: RestoreSnapshotReq) returns ControlReply
}

node atomic DbClient  { tipc type=0x80010020 instance=0
    ports { server client_api  provides DbClientIf  } }
node atomic DbManager { tipc type=0x80010021 instance=0
    ports { server manager_api provides DbManagerIf } }
```

(Config values are stored as binary protobuf bytes; `ConfigSnapshot` carries the
serialized message + its digest, the client unpacks. Messages like
`ConfigSnapshot`/`GetConfigReq`/… are declared in the same `.art` so gen-app
emits them into the generated proto.)

**Config updates are delivered as a CAST to the node, handled by the runtime —
not a per-app handler.** `WatchConfig` only *arms* the watch; when services/db
sees an etcd change it casts a `ConfigUpdated{config, FieldMask changed}` to the
owning node, and the **framework `handle_cast` in GenServer.hh applies it** —
exactly like `LogLevelPush` / `TraceControlPush` today. So the runtime grows a
third standard config-service cast (trace, log, **config**); the app never writes
a watch loop or a config handler, it just reads `get_config()` / overrides
`on_config_update(cfg, mask)` if it wants the live hook. This is why the client
`WatchConfig` op returns a plain ack — the payload arrives out-of-band via the
same config-service receiver every reporting node already binds.

### Step 2 — params: per-FC JSON + runtime config singleton (independent slice)

This slice has NO services/db / etcd dependency and can land first, on its own:

- **Param emitter** (`gen-app`): walk each FC's nodes, emit
  `/ROOT/<machine>/config/<fc>.json` with one section per node name (reuse the
  `generate_etcd_schema` param walk, write per-FC JSON). Stage it next to the
  binaries (like executor.json/netgraph.json).
- **Runtime config reader + singleton** (`platform/runtime`): a small JSON
  reader + a process-wide `Config` singleton; `get_config()` accessor with typed
  getters (`node(name).u32(key, default)`, `.str(...)`, `.boolean(...)`).
- **Generated main.cc** initializes the singleton early in boot from
  `$THEIA_ROOT_DIR/<machine>/config/<fc>.json` (supervisor sets the env from
  `root_dir_`/`machine_name`; `<fc>` = component name). The supervisor must add
  `THEIA_ROOT_DIR`/machine to the child env if not already there.
- **Nodes pull values** via `get_config().node(kNodeName)...` in ctor/`init()`.
- Test: an FC with a `params {}` block reads a non-default value from a staged
  JSON at boot; a missing key falls back to the `.art` default.

### Step 3 — `const` param modifier + config schema registry (gen-db)

- Add `const` to `NodeParam` in the grammar; gen-app emits const members
  (documents read-only; the JSON model has no live writer anyway).
- `artheia gen-db` walks packages → **config**-schema registry (proto digest per
  node-`config` type). `RegisterSchema`/`ListSchemas`. (params are NOT here —
  they're the Step 2 JSON.)

### Step 4 — runtime ConfigUpdated cast + GetConfig/WatchConfig + FieldMask

- **Runtime side first:** add the framework `ConfigUpdated` message to the
  runtime proto (sibling of TraceControlPush/LogLevelPush) and a
  `handle_cast(ConfigUpdated)` in GenServer.hh that decodes the bytes to the
  node's `config` type, swaps `shared_ptr<const Config>`, honors the per-field
  STATIC/HOT_RELOAD/RESTART_REQUIRED class, and calls the optional
  `on_config_update(cfg, mask)` hook. Generated main.cc `register_cast`s it on
  the node's config-service receiver (same place LogLevelPush/TraceControlPush
  register).
- `DbClientIf.GetConfig` returns the node's config as binary protobuf (client
  unpacks); `#version`/digest stripped, migrated to the caller's schema.
- `DbClientIf.WatchConfig` arms an etcd watch (`WithPrevKV`); on each event
  services/db parses prev + new as the node's proto (schema-registry digest),
  computes the typed FieldMask diff, and **casts `ConfigUpdated` directly to the
  node** (not via supervisor — config is durable in etcd).
- Test: arm a watch, change one field, assert the cast lands in the node's
  framework handler, the FieldMask names exactly that path, and the snapshot is
  the full new value.

### Step 5 — lazy migration (read + write) by schema digest

- `GetConfig` at digest D: stored digest = D → return; else run transform chain
  to D, strip version, return. Chains automatically.
- `PutConfig` at an older digest maps-forward before persisting.
- Transforms in `services/db/migrations/<node>/<from>_to_<to>.cc`.
- Test: write under v1, register v1→v2, GetConfig at v2 returns migrated; chain
  v1→v2→v3.

### Step 6 — MigrateBulk + crash-safe resume

- Rewrite every value under a node's config prefix through the chain; streaming
  progress; lease-bound resume marker at
  `/theia/db/migrations/<node>/<from>_<to>`.
- Test: 1000 values, kill -9 at ~50%, restart, completes, no duplicates.

### Step 7 — snapshot / restore

- `Snapshot(label)` → `etcdctl snapshot save` (or Maintenance gRPC), tagged at
  `/theia/db/snapshots/<label>`. `RestoreSnapshot(label)` restores.
- Test: Snapshot, mutate, Restore, state matches.

### Step 8 — wire into rig + supervisor manifest

- Add `services/db` to `services/manifest/service.py`, pinned to the
  central host (where etcd lives). gen-app ChildSpec.
- Smoke: supervisor starts services/db → db connects to etcd → ping OK.
- com bridge so off-host callers reach Db.* without knowing where etcd is
  (defer unless a caller needs it).

### Step 9 — example app round-trips Db (acceptance gate)

- An app declares `params {}` + `config <Msg>`; reads a param at boot; gets a
  live `ConfigUpdated` when its config changes; never links etcd.
- Example at `demo/db_example/`; Robot scenario at
  `testing/rf_theia/scenarios/services_db/round_trip.robot`.

## Out of scope (handled elsewhere or deferred)

- **OTA / A-B slots / health gates** — Puppet handles deploy.
- **Multi-machine MigrateBulk / multi-node etcd cluster + TLS** — single-node
  localhost for now.
- **Apps connecting etcd directly** — explicitly forbidden; services/db is the
  sole client by design.
- GUI tabs over etcd / supdbg --backend etcd — independent, work over com today.

## Files this task will touch (best-effort inventory)

New:

- `system/services/db/{package.art, component.art}` — the `.art` declaration
  (DbClientIf + DbManagerIf interfaces, DbClient + DbManager nodes, messages).
  **proto is GENERATED by gen-app — no hand-written `db.proto`.**
- `services/db/impl/<Node>_handlers.cc` + the gen-app lib/main slices
  (`gen-app --kind fc` on the component.art) + impl/etcd client wrapper (the
  sole etcd-cpp-apiv3 link) + `services/db/BUILD.bazel` + per-step tests
- `services/db/migrations/<node>/<from>_to_<to>.cc`
- `platform/runtime/` — (a) params: config-file reader + `Config` singleton +
  `get_config()`; (b) config: the framework `ConfigUpdated` proto message
  (sibling of TraceControlPush/LogLevelPush) + `handle_cast(ConfigUpdated)` in
  `GenServer.hh` + the `register_cast` in the generated main.cc template
- `artheia/artheia/generators/` — param-JSON emitter (per-FC
  `/ROOT/<machine>/config/<fc>.json`, reuses the `etcd_schema.py` param walk) +
  `gen-db` (config-schema digest registry — config only)
- `demo/db_example/`, `testing/rf_theia/scenarios/services_db/round_trip.robot`
- `docs/services-db.md` (user guide: params vs config, const, schema digest,
  ConfigUpdated + FieldMask, transform shape)

Modified:

- `artheia/artheia/grammar/artheia.tx` — `const` on `NodeParam`; per-field
  dynamic-update annotation on config (option or `.art` tag)
- generated `main.cc` template — init the config singleton early in boot +
  `register_cast<ConfigUpdated>` on the config-service receiver
- `platform/runtime/include/GenServer.hh` — `handle_cast(ConfigUpdated)`
- `platform/supervisor` — set `THEIA_ROOT_DIR` + machine in the child env (so
  main.cc can locate `<machine>/config/<fc>.json`)
- `services/manifest/service.py`, `demo/manifest/rig.py`

## Status

Not started. **Step 2 (params: per-FC JSON + runtime config singleton) is the
natural first slice and is fully independent of etcd/services-db** — it lands the
static-deploy path on its own. The config/watch/migration machinery (Steps 1,
4–9) reintroduces etcd: the supervisor no longer talks to etcd, so services/db
becomes the first and only etcd client in the system.
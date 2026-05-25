# services/db — state-shape gatekeeper

Aggregates: task #239 (TaskList) + Phase 6 of
[`BACKLOG/etcd-state-backbone.md`](../BACKLOG/etcd-state-backbone.md).

## Goal

Stand up `services/db` as the typed, versioned state gatekeeper on top
of etcd. Apps write/read through services/db (not raw etcd); the
service handles schema versioning, lazy migration on read, and
bulk-rewrite fallback for reshapes that don't fit the lazy model.

The boundary is sharp: **services/db owns data shape; Puppet owns
deploy.** OTA / A-B slots / health gates are explicitly out of scope.

## What's already in place

- **etcd backbone live** (#240, #243): single-node localhost daemon
  at 127.0.0.1:2379, supervisor publishes
  TreeSnapshot/HealthBeacon/ChildState under `/theia/state/<machine>/`,
  SupervisionEvent under `/theia/events/<machine>/<ts>-<seq>`.
- **GUI Table Viewer** (#241, #242): supervisor-gui has an etcd
  panel that browses keys + watches a prefix.
- **etcd-cpp-apiv3 in repo manifest** (#241): C++ client library
  available for services/db to link against. Same client the GUI uses.

## What's NOT in place (this task)

- No `services/db` binary, no `.proto`, no `.art` declaration.
- No schema registry under `/theia/app/schema/`.
- No migration transforms.
- No snapshot/restore wrapper around `etcdctl`.

## Scope (subdivided into landable steps)

### Step 1 — proto + .art declaration

Define the gRPC surface in `services/db/proto/db.proto`:

```proto
service Db {
  // Schema registry.
  rpc RegisterSchema(RegisterSchemaRequest) returns (ControlReply);
  rpc ListSchemas(ListSchemasRequest) returns (SchemaList);

  // Typed KV (proto value, app-tagged, version-tagged).
  rpc Put(PutRequest) returns (ControlReply);
  rpc Get(GetRequest) returns (GetReply);    // lazy migration on read
  rpc Watch(WatchRequest) returns (stream WatchEvent);

  // Bulk operations.
  rpc MigrateBulk(MigrateBulkRequest) returns (stream MigrateProgress);

  // Operational snapshot / restore (wraps `etcdctl snapshot`).
  rpc Snapshot(SnapshotRequest) returns (ControlReply);
  rpc RestoreSnapshot(RestoreSnapshotRequest) returns (ControlReply);
}
```

Mirror in `services/db/system/package.art` so gen-app can build the
service the same way every other FC is built (#368 layout).

### Step 2 — minimal Put/Get round-trip

- `services/db/src/main.cc` + `db_daemon.cc`: gRPC server, etcd-cpp-apiv3
  client, `Put` writes `/theia/app/data/<app>/v<v>/<key>`, `Get` reads
  the matching path.
- No migrations yet — `Get(v=X)` only returns keys at `v<X>`. Missing
  key → NOT_FOUND.
- Bazel BUILD.bazel; integration test in `services/db/test/` round-tripping
  a `PutRequest{app="demo", schema_v=1, key="foo", value=...}`.

### Step 3 — schema registry

- `RegisterSchema(app, v, proto_digest)` writes
  `/theia/app/schema/<app>/v<n>`.
- `ListSchemas(app)` returns versions known + their digests.
- Reject `Put` when no schema is registered for `(app, v)`.
- Test: register v1, register v2 with different digest, list returns
  both, put-with-unknown-v returns INVALID_ARGUMENT.

### Step 4 — lazy migration on read

- Migration transforms live in
  `services/db/migrations/<app>/v<from>_to_v<to>.cc`,
  registered at daemon startup (static-init registrar pattern; same
  shape as platform/runtime/trace/trace_decoder_protos.cc).
- `Get(app, v=2, key)` flow:
  1. Read `v2/<key>` → return.
  2. Else read `v1/<key>` → apply v1→v2 transform → return.
  3. Else NOT_FOUND.
- Chains migrations (v1→v2→v3) automatically.
- Test: write v1, register v1→v2 transform, Get with v=2 returns
  migrated value. Verify chain v1→v2→v3.

### Step 5 — MigrateBulk + crash-safe resume

- `MigrateBulk(app, from_v, to_v)` reads every key under
  `v<from>/`, applies the chain of transforms, writes to `v<to>/`.
- Streaming RPC progress (count + percent).
- Resumable marker: lease-bound key at
  `/theia/app/migrations/<app>/<from>_<to>` carries last-processed
  key. On restart, daemon checks the marker and resumes.
- Test: write 1000 v1 keys, start MigrateBulk, kill -9 at ~50%,
  restart, completes to 100%, no duplicates.

### Step 6 — snapshot / restore

- `Snapshot(label)` calls `etcdctl snapshot save` (or the etcd Maintenance
  gRPC), tags at `/theia/db/snapshots/<label>` with timestamp + caller.
- `RestoreSnapshot(label)` calls `etcdctl snapshot restore`. Supervisor
  + services/db restart hooks: supervisor's own state under
  `/theia/state/` re-populates on its next 1Hz publish.
- Test: Snapshot("pre"), write 100 keys, RestoreSnapshot("pre"), keys
  gone, supervisor state still populated within ~2s of restart.

### Step 7 — wire into rig + supervisor manifest

- Add `services/db` to `services/manifest/service.py` (pin to
  central_host where etcd lives).
- Generate the supervisor's ChildSpec via `artheia gen-app --kind fc`
  (#370 layout: spec on `services/db/system`, impl on `services/db`).
- Update `executor.yaml` smoke: supervisor starts services/db,
  services/db connects to etcd, ping RPC returns OK.
- Real services/com co-deployment: optionally add a thin gRPC bridge
  on services/com that proxies Db.Get/Put to remote callers (so apps
  don't need to know whether they're on the same machine as etcd).
  **Defer to a separate task unless a caller actually needs it.**

### Step 8 — example app round-trips Db

This is the acceptance gate from the etcd-backbone doc:

1. `services/db` running locally; `RegisterSchema(myapp, 1, …)` +
   `Put + Get` round-trip via gRPC.
2. Write a v1 key, read with `schema_v=2`, services/db transparently
   runs the v1→v2 transform on the read path.
3. `MigrateBulk(myapp, 1, 2)` rewrites 1000 keys at v1 to v2;
   kill -9 mid-migration, restart, resume to completion (lease
   marker at `/theia/app/migrations/myapp/1_2` survives).
4. `Snapshot("pre-bulk")` + `RestoreSnapshot("pre-bulk")` round
   trips the entire datastore.

Example app lives at `demo/db_example/` (cc_binary), wired into the
demo rig under central_host. Robot Framework scenario at
`testing/rf_theia/scenarios/services_db/round_trip.robot`.

## Out of scope (handled elsewhere or deferred)

- **OTA / A-B slots / health gates** — Puppet handles deploy; the
  data state lives behind services/db. Cross-deploy state-schema
  delta detection is a future "OTA over Puppet" story.
- **Multi-machine MigrateBulk** — single-machine for now (etcd is
  single-node localhost). Fleet coordination is a later layer.
- **Multi-node etcd cluster + TLS** — out of scope per
  etcd-state-backbone.md's "Out of scope" section.
- **Tab migration to etcd reads** (Phase 5 of etcd-state-backbone) —
  independent of services/db; the GUI tabs already work over
  services/com. Track as a separate task if/when the tester wants it.
- **supdbg --backend etcd** (Phase 3 of etcd-state-backbone) —
  independent; supdbg works over services/com today. Nice-to-have,
  not blocking.

## Files this task will touch (best-effort inventory)

New:
- `services/db/proto/db.proto`
- `services/db/system/{package.art, component.art}`
- `services/db/{include, src}/db_daemon.{hh, cc}` + `main.cc`
- `services/db/BUILD.bazel` (Bazel) + per-step test files
- `services/db/migrations/<app>/v<from>_to_v<to>.cc` (one or more)
- `demo/db_example/` (cc_binary)
- `testing/rf_theia/scenarios/services_db/round_trip.robot`
- `docs/services-db.md` (user guide — typed Put/Get, schema registry,
  migration transform shape)

Modified:
- `services/manifest/service.py` — add services/db FC entry
- `demo/manifest/rig.py` — pin to central_host
- (any executor.yaml regenerated, not hand-edited)

## Status

Not started. Steps 1-2 (proto + minimal Put/Get) are the natural
first slice — small enough to land in one session, validates the
etcd-cpp-apiv3 link + the gRPC bridge before the migration mechanism
goes in.

# Config schema migration (services/per)

How a node's **runtime config** evolves across versions, and the tooling that
records, tests, and ships a migration. Read this when you change a `config
<Msg>` shape, or write/extend a `transform.json`. The full design rationale is
in [docs/artheia/transform.md](../../../artheia/transform.md).

## The model in one paragraph

`services/per` is the **config gatekeeper**: each node binds one `config <Msg>`,
stored as **binary proto bytes** at `/theia/config/<node>` in etcd, tagged with
a content-hash **digest** of its shape. When the shape changes, the digest
changes — and a stored value whose digest ≠ the current schema digest needs a
**migration**. Migrations are strictly **1:1 payload evolution** (one config per
node); storage-topology (key moves / splits / merges) is explicitly out of
scope. Migrations chain `v1→v2→v3` from adjacent `from_digest→to_digest` steps
(BFS in `MigrationRegistry`), and run **lazily on read** (`GetConfig`) and in
**bulk** (`PerManager.MigrateBulk`).

## Two engines, one rule-set — keep them lockstep

The *same* rules run in two places that **must agree**:

| engine | where | works on | role |
| --- | --- | --- | --- |
| `tools/migrate/migrate.py` | dev box | decoded **JSON** | architect's design/test bench — record + preview a migration |
| the dlopen'd plugin | `services/per` (MigrateBulk + lazy read) | the **nanopb struct** (no JSON, no libprotobuf) | the runtime — fast |

To extend the rule vocabulary, edit `migrate.apply_rule` **and**
`transform_codegen._emit_rules` **together**. A regression test should assert
they produce identical output on a real config (the lockstep invariant).

## The tool chain

```
.art (config <Msg>)                                  ← evolve the shape
  │  artheia gen-schema <component.art> --out schema_vN.json   (config_type → digest + fields)
  ▼   (keep the PREVIOUS schema_v{N-1}.json — the diff baseline)
artheia gen-migration --from schema_v{N-1}.json --to schema_vN.json --out migration/
  │   → one <node>_v1_to_v2.json per CHANGED config (auto from/to digest +
  │     add/remove + same-tag RENAME heuristic + custom stub for type changes;
  │     each guess flagged in a `_review` note) + the BUILD plugin entries.
  ▼   ← REVIEW the scaffold: confirm renames, set add-defaults, fill custom hooks.
tdb get-snapshot <label> --schema schema_vN.json     → snapshot.json (decoded, JSON)
  │  migrate.py --snapshot snap.json --transform <node>_v1_to_v2.json --out next.json
  ▼   ← DESIGN: eyeball the v_{n+1} snapshot, iterate on the transform .json
artheia gen-transform <node>_v1_to_v2.json --schema schema_vN.json --out <node>_v1_to_v2.cc
  │   → plugin.cc (+ <node>_v1_to_v2_custom.cc if a {custom} rule)
  │  bazel build //migration:libper_migrate_<node>.so   (the managed BUILD entry)
  ▼
PerManager.MigrateBulk(config_type, from_digest, to_digest, plugin_so)  ← rewrite the store
  + the same plugin serves lazy migration on GetConfig read
```

`gen-migration` is the diff-to-scaffold step that removes the per-node toil:
positional proto tags mean the diff is by INDEX — same-tag/different-name →
`rename`, appended → `add`, truncated tail → `remove`, same-tag/different-type →
a `custom` hook stub. Renames + type changes are HEURISTICS, so every emitted
transform carries a `_review` list of what it guessed; confirm before codegen. A
config with an unchanged digest emits nothing; a config only in the NEW schema is
a fresh binding (skipped — no stored old value). The BUILD region between the
`# >>> gen-migration plugins (managed) >>>` markers is MERGED (a later diff
doesn't drop an earlier diff's nodes).

## Config-field defaults — declare once (same as `params`)

proto3 has NO field defaults, so a config field's "default" used to live in two
hand-typed places: the migration `add` rule and the node's C++ init. You can now
declare it ONCE on the `.art` config message field — the SAME `= value` syntax a
node `params` entry uses:

```art
message CounterConfig {
    uint32 step       = 1
    uint32 max_value  = 100
    bool   wrap       = false
    string label      = "counter"
    uint32 hysteresis = 3
}
```

The `= value` is a DEFAULT, not a proto tag (tags stay positional — body order).
It is **not part of the wire shape / digest** (declaring or changing a default
does NOT trigger a migration). Three consumers read the one declaration:

- **gen-schema** records it per field (`"default": 3`) — the single source.
- **gen-migration** fills an `add` rule's default from it (so a newly-introduced
  field migrates to its declared value, not a neutral 0). Falls back to neutral
  when undeclared.
- **gen-config-defaults** emits a per-node first-boot seed:
  `artheia gen-config-defaults <component.art> --out config_defaults.json` →
  `{prototype: {config_type, digest, values}}`. A seeder (`migration/seed.py
  defaults --defaults config_defaults.json`) PutConfigs the declared values for
  any node with NO stored value — so a fresh node boots at its declared
  defaults instead of proto3 zeros. Nodes with an existing value are untouched
  (idempotent); fields without a declared default stay zero (opt-in).

Declaring a default is opt-in per field; a config with none behaves exactly as
before (neutral migration default + proto3 zeros).

## transform.json

```json
{
  "config_type": "CounterConfig",
  "from_digest": "cfg_aaa",
  "to_digest":   "cfg_bbb",
  "rules": [
    {"op": "rename",    "from": "$.label", "to": "$.tag"},
    {"op": "add",       "field": "hysteresis", "default": 3},
    {"op": "remove",    "field": "dead"},
    {"op": "set",       "field": "step", "value": 10},
    {"op": "copy",      "from": "$.a", "to": "$.b"},
    {"op": "transform", "path": "$.status", "map": {"ACTIVE": "enabled"}, "default": "off"},
    {"op": "custom",    "fn": "fixup_counter"}
  ]
}
```

- Field refs are **JSONPath** — the dotted nested-object subset (`$.a.b`,
  jq-consistent; no wildcards). A flat `$.x` is a struct member; a nested
  `$.a.b` over a sub-message isn't a flat member → use a `custom` hook.
- `transform` is a **value/enum remap**: unmapped values pass through, or take
  `default`.
- Optional header `"cardinality": "1:1"` / `"maxFanout": 1` — anything else is
  **rejected** (topology is out of scope; `migrate.validate_cardinality`).

## `{custom}` — the code-hook sidecar

A rule the declarative ops can't express is a `custom` hook. `gen-transform`
emits, in the main plugin:

```c
extern "C" void fixup_counter(const Cfg* in, Cfg* out);   // you implement this
...
fixup_counter(&from, &to);
```

and writes a **WRITE-ONCE** sidecar `<out>_custom.cc` with the typed stub —
never clobbered on regen (like gen-fc's `impl/`). You implement the body on the
typed nanopb structs. So 100% is expressible, ~90% generated. (The whole plugin
IS C, so even a fully hand-written `per_register_migrations` is valid.)

## Deleting a field: `reserved`

ART message tags are **positional** (body order = proto tag 1..N). Deleting a
field would shift every later tag down and break the wire-compat of values
already stored. Instead, replace the field with a `reserved` marker where it
was:

```art
message CounterConfig {
    uint32 step
    reserved label      // was a string; deleted. Holds tag 3.
    bool   wrap         // stays tag 4
}
```

The generator emits `reserved 3;` and no field, so tag 3 is never reused and
`wrap` keeps tag 4. **Keep fields in a stable order** — order is the tag.

## Plugin build + ABI

The transform plugin is a standalone `.so` per dlopens — NOT linked into the per
binary. Build it `cc_binary(linkshared=True, linkstatic=False)` against
`//services/per/impl:migration_registry` (the C ABI header only). The ABI is
`services/per/impl/migration_plugin_api.h`:
`extern "C" void per_register_migrations(const per_migration_api*)` →
`api->add_edge(host, from_digest, to_digest, transform_fn)`. Strings the
transform touches must be `.options`-pinned to `char[]` (else `pb_callback_t`).

## Migrations are PER NODE

A migration is keyed by **config_type**, and each node binds its own
`config <Msg>` — so each node carries an **independent** v1→v2 evolution with its
**own** `from_digest`/`to_digest` and its own rule set. A `transform.json` (and
its generated plugin) covers exactly one config_type; `MigrateBulk` is invoked
per config_type. There is no cross-node merge — per is strictly 1:1 per node, so
"fold p1/p2/p3 fields into p4" is modeled as a *within-p4* schema bump (p4's own
v1→v2 that ADDs those fields), not a 3→1 consolidation. The demo proves three
distinct per-node migrations side by side:

| node | config | rule kind |
| --- | --- | --- |
| `counter` | CounterConfig | **add** a field (`hysteresis`) |
| `observer` | ObserverConfig | **rename** (`name`→`tag`, same tag number) |
| `demo_fsm` | P4Config | **rename + add** (the consolidation target) |

A rename that keeps the field NUMBER is just a carry — the v1 bytes already
decode into the new member (gen-transform emits a no-op note, not `from.<old>`).

## End-to-end: the verified dev workflow

The whole loop, proven against a live etcd-backed per (artefacts under
`migration/`, run dir `install/central/`):

```sh
# 1. clean etcd + schema of the v1 shapes
ETCDCTL_API=3 etcdctl --endpoints=127.0.0.1:2379 del --prefix /theia/config/
artheia gen-schema apps/system/apps/component.art --out migration/schema_v1.json
theia install central && theia start central        # ← theia start/stop run install/

# 2. seed v1 values (PutConfig), change some, snapshot
python migration/seed.py seed --schema migration/schema_v1.json
tdb get-snapshot snap_v1 --schema migration/schema_v1.json
theia stop central

# 3. evolve the .art (+ apps.options for new string fields), regen the FC,
#    gen-schema of the v2 shapes, author migration/<node>_v1_to_v2.json
artheia gen-schema apps/system/apps/component.art --out migration/schema_v2.json
theia start central

# 4. OFFLINE preview (design bench)
tdb get-snapshot snap_v1b --schema migration/schema_v1.json
python tools/migrate/migrate.py --snapshot snap_v1b.json \
    --transform migration/<node>_v1_to_v2.json --out snap_v2_offline.json

# 5. ONLINE: gen plugin, build the .so, MigrateBulk on the live per
artheia gen-transform migration/<node>_v1_to_v2.json \
    --out migration/<node>_v1_to_v2.cc --schema migration/schema_v2.json
bazel build //migration:libper_migrate_<node>.so
#   PerManager.MigrateBulk(config_type, from_digest, to_digest, plugin_so=...)

# 6. COMPARE offline == online  (the lockstep invariant), then change a v2
#    value, theia install + start, and confirm the migration survives a restart.
```

`theia start [machine]` / `theia stop [machine]` run the staged supervisor from
`install/<machine>/` (detached + pidfile); they replace the hand-typed
`THEIA_SUPERVISOR_MANIFEST=… ./supervisor`.

## RF wrapper — parametrized migration tests

`testing/rf_theia/scenarios/services/per_migration/` wraps the whole flow in a
Robot Framework module, parametrized by node:

- `per_migration_lib.py` — a `MigrationCase` per node (`node`, `config_type`,
  `from`/`to` digest, `rules`, `seed`, `expect`) in the `CASES` list. Add a node
  there and both its per-node test and the sweep pick it up.
- Keywords: `Migrate Offline` (runs the real `migrate.py`), `Assert Migrated
  Value`, `Assert Nanopb Roundtrip` (decodes v1 bytes with the v2 struct — the
  plugin's `pb_decode` half, hermetic), `Assert Digest Bumped`, and
  `Migrate Online And Compare` (live: seed → build+load plugin → `MigrateBulk` →
  assert online == offline; retries the per-restart TIPC race).
- `per_migration.robot` — one test per node, tagged `hermetic` (no stack) or
  `live` (needs `theia start` + etcd).

```sh
cd testing
PYTHONPATH=. ../.venv/bin/robot --include hermetic rf_theia/scenarios/services/per_migration/
PYTHONPATH=. ../.venv/bin/robot --include live    rf_theia/scenarios/services/per_migration/
```

## Gotchas (battle-tested in the e2e run)

- **The plugin `.so` MUST be self-contained.** per dlopens with `RTLD_NOW` and
  exports neither nanopb nor the proto descriptors, so the plugin needs them IN
  it: compile `demo.pb.c` straight in as a `src`, link nanopb statically
  (`linkopts = ["-l:libprotobuf-nanopb.a"]`), and depend on the proto only via a
  **header-only** target (a compiled proto cc_library adds a `DT_NEEDED` on a
  shared lib that isn't on per's dlopen path → `dlopen failed`). See
  `migration/plugin.bzl` for the macro.
- **`dlerror()` clears on read** — read it ONCE. The loader used to read it
  twice in `x ? x : "unknown"`; the second call returned null and
  `std::string + null` segfaulted per. (Fixed in `migration_plugin.cc`.)
- **String config fields must be `.options`-pinned** to `char[]` (else
  `pb_callback_t`, which the struct-copy codegen can't assign and snapshots
  can't decode). Add `<pkg>.<Msg>.<field> max_size:N` to the FC's `.options`.
- **Repeated `MigrateBulk` across probe sessions can hit the TIPC stale-binding
  race** (per crashes/restarts, a fresh probe load-balances onto a dead port).
  It's a runtime hazard, not a migration bug — the RF live keyword retries on a
  fresh probe ([[project-probe-connect-stale-bindings]]).

## Operations cheat sheet

`Snapshot` / `RestoreSnapshot` are **config-prefix scoped** (just
`/theia/config/`, a live re-put — NOT a full etcd snapshot). `RegisterSchema` /
`ListSchemas` track digests. etcd revisions are **global** (CAS uses the actual
rev, not 1/2/3). The store backend is etcd by default; `THEIA_PER_BACKEND=mem`
forces in-memory (tests).

## See also

- Design + decisions: [docs/artheia/transform.md](../../../artheia/transform.md)
- Generators: `artheia/artheia/generators/{config_schema,migration_diff,transform_codegen}.py`
- Reference applier: `tools/migrate/migrate.py`
- per internals: `services/per/impl/` (etcd_store, migration_registry,
  migration_plugin, schema_registry, snapshot_ops)
- Worked example + artefacts: `migration/` (schemas, per-node transforms +
  plugins, `seed.py`, `plugin.bzl` macro, `README.md`)
- RF suite: `testing/rf_theia/scenarios/services/per_migration/`

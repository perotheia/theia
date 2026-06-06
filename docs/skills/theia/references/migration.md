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

## The 4-tool chain

```
.art (config <Msg>)                                  ← evolve the shape
  │  artheia gen-schema <system.art> --out schema.json   (config_type → digest + fields)
  ▼
tdb get-snapshot <label> --schema schema.json        → snapshot.json (decoded, JSON)
  │  migrate.py --snapshot snap.json --transform t.json --out next.json
  ▼   ← DESIGN: eyeball the v_{n+1} snapshot, iterate on t.json
artheia gen-transform t.json --schema schema.json --out plugin.cc
  │   → plugin.cc (+ plugin_custom.cc if a {custom} rule)
  │  build a cc_binary .so (linkshared) — copy services/per/migrations/BUILD.bazel
  ▼
PerManager.MigrateBulk(from_digest, to_digest, plugin_so)   ← RUNTIME: rewrite the store
  + the same plugin serves lazy migration on GetConfig read
```

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
never clobbered on regen (like gen-app's `impl/`). You implement the body on the
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

## Operations cheat sheet

`Snapshot` / `RestoreSnapshot` are **config-prefix scoped** (just
`/theia/config/`, a live re-put — NOT a full etcd snapshot). `RegisterSchema` /
`ListSchemas` track digests. etcd revisions are **global** (CAS uses the actual
rev, not 1/2/3). The store backend is etcd by default; `THEIA_PER_BACKEND=mem`
forces in-memory (tests).

## See also

- Design + decisions: [docs/artheia/transform.md](../../../artheia/transform.md)
- Generators: `artheia/artheia/generators/{config_schema,transform_codegen}.py`
- Reference applier: `tools/migrate/migrate.py`
- per internals: `services/per/impl/` (etcd_store, migration_registry,
  migration_plugin, schema_registry, snapshot_ops)
- E2E test (backlog): `docs/tasks/BACKLOG/config-migration-e2e-test.md`

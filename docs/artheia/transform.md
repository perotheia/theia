model migrations as a **versioned DAG of document transformations** with operations `{moveKey, moveField, transform, split, merge}` plus an escape hatch `{custom}`. That is roughly the point where systems like Kubernetes CRD conversion,

Essential split: **schema evolution + data migration for PROTO documents stored in a KV store**. This problem has been solved in Kubernetes CRDs and Protobuf schema registries.

The key challenge is that you have:

1. **Storage schema** (paths/keys in etcd)
2. **Object schema** (PROTO structure)
3. **Versioned migrations**
4. **Cardinality changes** (1→N, N→1)
5. **Data transformations**
6. **Referential rewrites**

A pure declarative JSON transform language works well for ~80% of migrations, but some migrations inevitably require custom code.


---

Protobuf Evolution

Handled automatically.

```
add field
rename field
remove field (reserve tag)
default values
optional fields
```

---

Storage Migration DAG

Handled by your transform.json.

```
moveKey
split
merge
cross-document references
fan-out
path changes
payload reshaping
```


These operate on decoded protobuf objects.

```
{
  "op": "split",
  "source": "/users/*",
  "outputs": [
    "/users/${id}",
    "/addresses/${id}"
  ]
}
```

---

Engine flow:


```
read protobuf blob
deserialize using old schema
apply migration DAG
serialize using current schema
write new keys
```


That separation keeps protobuf evolution simple and leverages what Protobuf already does well, while reserving your migration framework for structural changes that Protobuf cannot represent.


---

# Recommended Pattern

Introduce a new field.

```protobuf
message Address {
  string city = 1;
}

message User {
  string name = 1;

  string city = 2;       // old
  Address address = 3;   // new
}
```

Read logic:

```text
if address exists:
    use address.city
else:
    use city
```

After all data upgraded:

```protobuf
message User {
  string name = 1;
  Address address = 3;

  reserved 2;
}
```

This is exactly how large protobuf deployments evolve.

On ART level we adding message { field reserved } syntaxes to keep evidence for skipped proto id. Field newer deleted.


# Existing Approaches


## Kubernetes CRD Conversion Outside Kubernetes

Not really.

Kubernetes CRD conversion is not a standalone framework.

It's an API pattern implemented inside Kubernetes:

```text
Stored Version
       |
Conversion
       |
Served Version
```

For example:

```text
v1alpha1
    |
    v
v1beta1
    |
    v
v1
```

When a client requests:

```http
GET /apis/example.com/v1/users
```

Kubernetes:

1. Reads stored object
2. Converts to requested version
3. Returns converted object

---

The conversion itself is usually custom Go code:

```go
Convert_v1_To_v2(...)
Convert_v2_To_v1(...)
```

or a conversion webhook.

What you can reuse is the architecture:

```text
Stored Version
        +
Conversion Graph
        +
Served Version
```

which is very applicable to your design.

---

# Migration file should support:

1. Key moves
2. Field moves
3. Field transforms
4. Object creation
5. Object deletion
6. Splits
7. Merges


---

# More Formal Grammar

Something like:

```json
{
  "from": 1,
  "to": 2,
  "operations": [
    {
      "op": "moveKey"
    },
    {
      "op": "renameField"
    },
    {
      "op": "copyField"
    },
    {
      "op": "removeField"
    },
    {
      "op": "transform"
    },
    {
      "op": "split"
    },
    {
      "op": "merge"
    },
    {
      "op": "custom"
    }
  ]
}
```

---

# Recommended Migration Model

Think of migration as operating on a collection of documents:

```text
/key1 -> objectA
/key2 -> objectB
```

Migration file should support:

1. Key moves
2. Field moves
3. Field transforms
4. Object creation
5. Object deletion
6. Splits
7. Merges

---

# Example Transform Grammar

Version header:

```json
{
  "from": 4,
  "to": 5,
  "operations": []
}
```

---

## Rename Field

```json
{
  "op": "renameField",
  "object": "User",
  "from": "$.name",
  "to": "$.fullName"
}
```

---

## Move Field

```json
{
  "op": "moveField",
  "object": "User",
  "from": "$.address.city",
  "to": "$.location.city"
}
```

---

## Key Relocation

Move object storage location.

```json
{
  "op": "moveKey",
  "from": "/users/*",
  "to": "/accounts/${id}"
}
```

Example:

```text
/users/123
```

becomes

```text
/accounts/123
```

---

## Value Transform

```json
{
  "op": "transform",
  "path": "$.status",
  "expression": {
    "ACTIVE": "enabled",
    "DISABLED": "disabled"
  }
}
```

---

## Split Object

Most important for cardinality changes.

Input:

```json
{
  "id": 1,
  "name": "john",
  "address": {...}
}
```

Output:

```text
/users/1
/addresses/1
```

Transform:

```json
{
  "op": "split",
  "source": "/users/*",
  "outputs": [
    {
      "key": "/users/${id}",
      "fields": [
        "id",
        "name"
      ]
    },
    {
      "key": "/addresses/${id}",
      "fields": [
        "address"
      ]
    }
  ]
}
```

---

## Merge Objects

```json
{
  "op": "merge",
  "sources": [
    "/user/*",
    "/profile/*"
  ],
  "target": "/account/${id}"
}
```

---

## Create References

Useful after splits.

```json
{
  "op": "addField",
  "path": "$.addressRef",
  "value": "/addresses/${id}"
}
```

---


## Database Migration Chains


Model:

```text
v1 -> v2 -> v3 -> v4
```

Migration files:

```text
001.json
002.json
003.json
```

Each migration only knows:

```text
from version N
to version N+1
```

Advantages:

* Simple
* Easy rollback
* Easy testing

Limitation:

Complex object restructuring becomes verbose.


---

# What Cannot Be Fully Declarative

Some migrations are fundamentally procedural.

Example:

```text
Order
    ->
Invoice
```

where invoice totals must be recomputed from historical tax rules.

Or:

```text
User.address.country
```

must be looked up from an external service.

A transform language cannot safely express arbitrary business logic.

For this reason most mature systems eventually add:

```json
{
  "op": "custom",
  "handler": "UserToInvoiceMigration"
}
```

and run code.

A practical rule:

* 90% of migrations should be declarative.
* 10% should allow code hooks.

---

# Additional Constraints Worth Encoding

Every operation should declare cardinality:

```json
{
  "op": "split",
  "cardinality": "1:N"
}
```

Possible values:

```text
1:1
1:N
N:1
N:N
```

This lets the migration engine:

* validate transformations
* estimate document growth
* detect accidental fan-out explosions

Example:

```json
{
  "maxFanout": 10
}
```

to prevent creating millions of documents accidentally.

---

# In conclussion

For an etcd-backed object store, I would  ETL mapping tools, and document databases converge after years of schema-evolution experience.


Separation is sound and is actually how many large systems evolve:

```text
Storage migration layer
    (etcd keys, document topology)

            +

Payload schema evolution layer
    (protobuf message evolution)
```

The storage migration handles:

```text
/users/123
    ->
/accounts/123
```

or

```text
User
    ->
User + Address
```

while Protobuf handles:

```protobuf
message UserV1 {
  string name = 1;
}
```

becoming

```protobuf
message UserV2 {
  string full_name = 1;
  int32 age = 2;
}
```

without caring where the blob lives.


---

# Theia decisions & implementation status

How the research above maps onto the Theia config-migration tooling
(gen-schema / tdb get-snapshot / migrate.py / gen-transform → the dlopen'd
MigrateBulk plugin). The two-layer separation (storage topology vs payload
evolution) is honored — but the **storage-topology layer is deliberately OUT
OF SCOPE** here.

## Scope: payload evolution ONLY — topology is NOT per's job

`services/per` is a **config gatekeeper: exactly one config per node**, stored
at a fixed key `/theia/config/<node>` (one key = one object, no arbitrary
paths, no `${id}` wildcards). So config migration is strictly **1:1 — payload
evolution within a node's single config**.

The storage-topology ops from the research — `moveKey`, `split`, `merge`,
`fan-out`, cross-document references — change the key→object **cardinality** and
have **no home in per**. They belong to a general document store, not a config
gatekeeper. The tooling **rejects** them: a transform declaring `cardinality`
other than `1:1`, a `maxFanout > 1`, or a topology `op`
(`moveKey`/`split`/`merge`/`fanout`) is a hard error
(`migrate.validate_cardinality`). This keeps the engine from ever exploding the
keyspace and keeps the abstraction ladder clean (node = thread, config =
one-per-node).

If a general doc-store with topology migration is ever needed, it's a
**separate service** with its own engine — not per.

## Versioning: content DIGEST, not version integers

The research uses monotonic version ints (`"from": 1, "to": 2`). Theia uses a
**content-hash digest** instead (`cfg_<sha256-of-ordered-fields>`, from
`gen-schema`). The digest is strictly better: it auto-detects "shape changed"
with no human bump and can't drift. A stored value's digest ≠ the current
schema digest is *exactly* when a migration is needed. Adjacent transforms are
named/keyed `<from_digest> → <to_digest>`; the registry chains them by BFS
(`v1→v2→v3` from adjacent edges) — the "each step knows only N→N+1" model from
the research.

## Field addressing: JSONPath (jq-consistent)

All rule field references are **JSONPath** — the dotted nested-object subset
(`$.address.city`, `$.flags.beta`), consistent with `jq`. No
wildcards/filters/collection ops (those imply topology). This reaches nested
config fields. `migrate.py` (`path_get/set/del`) and the generated C plugin
(`jp_*` JSON-pointer helpers) implement the SAME path semantics — verified
byte-for-byte identical on a nested rename + value-map.

## Operations — implemented

| op | meaning |
|----|---------|
| `rename {from,to}`  | move a (possibly nested) field |
| `add {field\|path, default}` | add if absent (no clobber) |
| `remove {field\|path}` | drop a field |
| `set {field\|path, value}` | overwrite a field |
| `copy {from,to}` | duplicate a field |
| `transform {path, map, default?}` | **value/enum remap** via a mapping table; unmapped → `default` (or unchanged) |

Each op may carry `"cardinality": "1:1"` (the only allowed value). A transform
may carry `"maxFanout": 1` (anything > 1 is rejected).

## The `{custom}` escape hatch — already free

The research's "10% need code hooks" (`{op:"custom", handler:...}`) is **already
the foundation, not an add-on**: every transform IS C — the
`transform.json → gen-transform → .cc → dlopen plugin` path. `gen-transform`
is declarative *sugar* over the code-hook layer. A migration that the
declarative rules can't express is just a hand-written plugin `.cc` exporting
`per_register_migrations` (the same ABI). 100% expressible, 90% generated.

## Engine flow (matches the research)

```
read config blob (etcd /theia/config/<node>)
  → decode FROM-schema proto
  → apply rule chain (JSONPath ops on the decoded object)
  → encode TO-schema proto
  → re-put (CAS on rev)        [MigrateBulk]   — and lazy on GetConfig read
```

## Deferred (worth building later)

- **`reserved <tag>` in ART message syntax** — record skipped proto field IDs
  so a deleted field's tag is never reused (the research's "reserve tag"
  recommendation). Needs a small grammar + gen-proto change.
- **Proto-bytes ↔ JSON bridge per config type** — the generated plugin
  currently round-trips the value as JSON (the example treats the stored bytes
  as JSON). The production bridge decodes the FROM-nanopb struct → JSON, applies
  rules, encodes the TO-nanopb struct, so it works on real proto-wire configs.
  The rule *engine* (JSONPath + ops) is done and lockstep-verified; this is the
  (de)serialization shim around it.


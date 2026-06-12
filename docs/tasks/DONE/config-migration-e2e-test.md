# Config-migration end-to-end test

The config-migration tooling chain is built + unit-tested (gen-schema,
tdb get-snapshot, migrate.py, gen-transform, the dlopen plugin, `reserved`
syntax). What's missing is a **full e2e migration test** against a live etcd-
backed `per`, exercising a real schema evolution.

## Scope

A scenario (Robot/rf-theia or a standalone test) that:

1. **Evolves a config shape.** Take CounterNode's `config CounterConfig`
   (`step/max_value/wrap/label`) at digest `cfg_v1`; change the `.art` (e.g.
   rename `label → tag`, add `hysteresis`, or `reserved` a deleted field) →
   `cfg_v2`. Regenerate the schema (`artheia gen-schema`).
2. **Seeds the store at v1.** Put several CounterConfig values at `cfg_v1` into
   the live per (etcd backend).
3. **Designs + previews the transform.** Author `transform.json`, run
   `tools/migrate/migrate.py` on a `tdb get-snapshot` to preview the v2 snapshot,
   assert the decoded result.
4. **Generates + builds the plugin.** `artheia gen-transform … --schema …` →
   `plugin.cc` (+ `_custom.cc` if the transform uses a `custom` hook; implement
   the stub). Build the `.so` (cc_binary linkshared, like
   services/per/migrations/example).
5. **Runs MigrateBulk.** `tdb`/probe → `PerManager.MigrateBulk(cfg_v1, cfg_v2,
   plugin_so=…)`. Assert every v1 value is rewritten to v2 (count + content).
6. **Verifies lazy read agrees.** A `GetConfig(want=cfg_v2)` on a still-v1 value
   uses the SAME loaded plugin (no double-work). Assert the decoded result
   equals the migrate.py preview — the **lockstep invariant** end-to-end.
7. **Verifies wire-compat after `reserved`.** A value stored before a field was
   `reserved`-deleted still decodes (the dead tag isn't reused).

## Why it matters

The pieces are individually verified, but the *seam* between them (snapshot
digest ↔ schema ↔ plugin ↔ store) is only proven on toy bytes so far. This test
proves a real architect workflow: evolve `.art` → preview → ship → migrate live.
It's also the natural home for the **lockstep guard** — migrate.py (JSON design
bench) and the gen-transform plugin (nanopb runtime) producing identical results
on a real config, as a regression so the two engines can't drift.

## Notes / gotchas (from building the tooling)

- Strings the transform touches must be `.options`-pinned to `char[]` (else
  `pb_callback_t` the struct-copy can't assign) — see demo.options.
- etcd revisions are GLOBAL, not per-key — CAS uses the actual rev.
- Stale TIPC co-bindings from repeated per restarts confound probe tests; kill
  per by PID + let the kernel reap before a clean run. A supervisor-spawned
  `./bin/per` may be an OLD binary — stop the supervisor + kill it before
  re-testing.
- See `docs/artheia/transform.md` (design + decisions) and the
  `project-config-migration-tooling` memory.

## Status

DONE. Verified end-to-end against a live etcd-backed per: per-node config
evolutions (counter add-field, observer rename, demo_fsm rename+add), offline
migrate.py preview, gen-transform plugin, MigrateBulk online, and the lockstep
invariant (offline == online). Wrapped as a parametrized Robot Framework suite
at `testing/rf_theia/scenarios/services/per_migration/` (3 hermetic + 1 live,
all green under the rf-theia MCP). Worked artefacts under `migration/`.

Three seam bugs surfaced + fixed along the way: per's dlerror double-read
segfault (migration_plugin.cc), the plugin .so not being self-contained
(migration/plugin.bzl: compile demo.pb.c in + static nanopb + header-only proto
dep), and gen-transform's `rename` codegen on a same-field-number rename
(artheia). Full writeup: docs/skills/theia/references/migration.md.

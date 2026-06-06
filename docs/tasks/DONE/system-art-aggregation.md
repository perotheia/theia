# Cross-file `import` in artheia .art files

## Why

`platform/system/system.art` should be the top-level system definition
as a composition-of-compositions:

```
package platform.system

import services.system.*
import demo.system.*

composition Platform {
    composition Services svc
    composition Demo3Way demo
    prototype Supervisor sup
    prototype Gateway gw
}
```

The grammar already accepts the nested form (`composition Foo bar` as a
CompositionElement — landed in the system-art task). What's missing is
**real cross-file resolution**: today `import services.system.*` is a
syntactic no-op, and every cross-package reference has to be matched by
an inline forward-decl stub in the same file.

## Precondition — DONE

Package layout aligned with directory paths under a virtual `system.*`
root, where `platform/system/` IS the workspace's `system` package
root. Through the symlinks every component slot now matches:

| filesystem (workspace view)                      | package                       |
|--------------------------------------------------|-------------------------------|
| `platform/system/services/<short>/package.art`   | `system.services.<short>`     |
| `platform/system/services/<short>/component.art` | `system.services.<short>`     |
| `platform/system/gateway/package.art`            | `system.gateway`              |
| `platform/system/gateway/component.art`          | `system.gateway`              |
| `platform/system/supervisor/package.art`         | `system.supervisor`           |
| `platform/system/supervisor/component.art`       | `system.supervisor`           |
| `platform/system/demo/package.art`               | `system.demo`                 |
| `platform/system/demo/component.art`             | `system.demo`                 |
| `platform/system/autosar/mlbevo_gen2/kcan/package.art`  | `system.autosar.mlbevo_gen2.kcan`  |
| `platform/system/autosar/mlbevo_gen2/fibex/package.art` | `system.autosar.mlbevo_gen2.fibex` |

(Component sources still live on disk at e.g.
`services/system/<short>/package.art` —  the aggregator symlinks
expose the unified view.)

**Two .art files per dir** (package/component split): per-component
`package.art` carries messages/interfaces/nodes; `component.art`
carries the composition. Inside `component.art`, the referenced nodes
are redeclared as **forward-decl stubs** until cross-file imports land
(LSP already prefers non-stubs).

Once `FQNImportURI` resolves `import system.services.com.*` to
`<root>/system/services/com/{package,component}.art`, the stubs can be
removed mechanically.

## Implementation outline (from research)

1. Rename the `Import` grammar attribute: `Import: 'import'
   importURI=QualifiedNameWithWildcard` (textX hard-codes the literal
   attribute name `importURI` as the discriminator for ImportURI scope
   providers — `textx/scoping/providers.py:316`).

2. In `artheia/model/loader.py`, register an `FQNImportURI` scope
   provider on the cached metamodel with:
   - `search_path` listing the workspace roots (`platform/system/`,
     `ARTHEIA_PLATFORM_SERVICES`, the importing file's dir).
   - An `importURI_converter(fqn) → list[Path]` that strips trailing
     `.*` and yields candidate paths (try `system.art` then
     `package.art` at each level).

3. **Audit existing .art files** — many have `import` lines that are
   currently no-ops; once resolution is live, broken/stale imports
   become hard parse errors. Dry-run before flipping the switch.

4. Update every `parse_string()` callsite to pass a real `file_name=` —
   textX's `GlobalModelRepository.pre_ref_resolution_callback` asserts
   on `_tx_filename`, so unfilenamed in-memory parsing crashes once
   ImportURI is active.

5. Restore `_default_art_root()` to point at `platform/system/services`
   (set during the system-art task, reverted before commit because the
   manifest loader still does `<root>/<short>/package.art`). With
   cross-file imports working, the manifest loader probably wants to
   import packages by FQN instead of walking the filesystem at all.

6. Drop the redundant per-component forward-decl stubs from
   `platform/system/system.art` (and others) once cross-file refs
   actually resolve. Write the three new files:
   - `services/system/system.art` — `composition Services` aggregating
     all 18 FC compositions
   - `demo/system/system.art` — `composition Demo3Way` (currently
     embedded in `demo/system/package.art`)
   - `platform/system/system.art` — the top-level
     composition-of-compositions

## What's already in place (from the system-art task)

- `CompositionRefDecl` grammar — `composition Foo bar` parses.
- `flatten_composition()` helper splices inner prototypes + connects
  into the parent verbatim (no instance-prefixing).
- Validators reject cycles, prototype collisions, and instance-name
  clashes.
- All five composition-walking generators (netgraph, app_composition,
  routing, plus their callers) use the flattener.
- 6 new tests cover the above (5 grammar + 1 generator).
- `platform/system/` symlinks consolidated:
  ```
  platform/system/
  ├── autosar     → ../../autosar/mlbevo_gen2_cmp_psp/system
  ├── demo        → ../../demo/system
  ├── gateway     → ../../gateway/system
  ├── odd_path_client → ../../vendor/odd_path_client/system
  ├── services    → ../../services/system    (new — aggregates 18 FCs)
  └── supervisor  → ../supervisor/system
  ```

## Known regression to fix in this task

The symlink consolidation broke the manifest pipeline: per-FC
`platform/system/<short>` symlinks no longer exist, but the artheia
manifest loader still looks for `<art_root>/<short>/package.art`. Fix
options:

- Point `_default_art_root()` at `platform/system/services` (one-line
  change, restores old behavior).
- Or move the manifest loader to use FQN-based import resolution once
  this task lands.

The latter is the better long-term answer.

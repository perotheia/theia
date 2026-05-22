on file structure level

tree -L 1  services/
services/
├── manifest
├── pero_cmp_gw_svc -\> chnage in 'repo/manifest' to platform/gateway
├── supervisor -\> move platform/supervisor
└── system

also

tree -L 1  autosar
autosar -\> vendor/autosar
├── demo - remove from 'repo/manifest'
└── mlbevo_gen2_cmp_psp - move from default manivest to .repo/local_manifests

on art level. relink all parts under platform/system to resolve dependenties in system definitions

platform/system

├── autosar -  autosar exports to reference signals
└── demo -\> ../../demo  - current RIG we working

link to -\> services - Platform components(FC).
├── com
├── core
├── crypto
├── diag
├── exec
├── fw
├── idsm
├── log
├── nm
├── osi
├── per
├── phm
├── rds
├── shwa
├── sm
├── tsync
├── ucm
└── vucm

├── gateway    - not FC, part of platform
|- supervisor/ - not FC, part of platform


# Resolution (2026-05-22)

## Filesystem layout

- `services/pero_cmp_gw_svc` → `platform/gateway` and
  `services/supervisor` → `platform/supervisor` were already done in
  the earlier pre-tasks (commits in workspace + `.repo/manifests`).
- `autosar/demo` and `autosar/mlbevo_gen2_cmp_psp` moved to
  `.repo/local_manifests/psp.xml` per the earlier
  `repo manifests` cleanup (still pending a `repo sync` to drop the
  legacy default manifest entries — flagged outside this task).
- `platform/system/` aggregator consolidated from 18 per-FC symlinks
  to one `services` aggregator plus demo/gateway/supervisor/autosar:

    platform/system/
    ├── autosar         -> ../../autosar/mlbevo_gen2_cmp_psp/system
    ├── demo            -> ../../demo/system/demo
    ├── gateway         -> ../../gateway/system
    ├── odd_path_client -> ../../vendor/odd_path_client/system
    ├── services        -> ../../services/system
    ├── supervisor      -> ../supervisor/system
    └── system.art

## Package layout (NEW: directory ↔ package alignment)

`platform/system/` IS the workspace's `system` package root. Every
`.art` file under the aggregator now declares a package matching its
directory path:

| filesystem (aggregator view)                            | package                              |
|---------------------------------------------------------|--------------------------------------|
| `platform/system/system.art`                            | `system`                             |
| `platform/system/services/system.art`                   | `system.services` (FC aggregator)    |
| `platform/system/services/<short>/{package,component}.art` | `system.services.<short>` (×18)   |
| `platform/system/gateway/{package,component}.art`       | `system.gateway`                     |
| `platform/system/supervisor/{package,component}.art`    | `system.supervisor`                  |
| `platform/system/demo/{package,component}.art`          | `system.demo`                        |
| `platform/system/autosar/mlbevo_gen2/kcan/package.art`  | `system.autosar.mlbevo_gen2.kcan`    |
| `platform/system/autosar/mlbevo_gen2/fibex/package.art` | `system.autosar.mlbevo_gen2.fibex`   |

## Per-component file split (NEW)

Each component dir contains two files:
- `package.art` — comments, package decl, messages, interfaces,
  `node atomic` decl(s).
- `component.art` — package decl, forward-decl stub of the local
  node(s), and the `composition` declaration.

The split mirrors how the manifest treats things: nodes ↔
SwComponents, compositions ↔ Executable/Process entries. The stubs
in `component.art` exist because textX is single-file today; once
cross-file imports land, the stubs go away.

## Artheia DSL additions (NEW)

- Grammar: `CompositionRefDecl` — a `composition` body element that
  names another composition as an instance (`composition Services svc`).
- `flatten_composition()` helper splices nested-composition prototypes
  + connects into the parent verbatim (no instance-name prefixing).
- Validators reject cycles, prototype collisions after flattening,
  and instance-name clashes.
- Generators (netgraph, app_composition, routing) walk the flattened
  view automatically.
- 6 new tests; full suite 60 passed, 5 skipped (LSP protocol-level).

## Follow-up tasks created

- `docs/tasks/TODO/system-art-aggregation.md` — wire textX
  `FQNImportURI` so `import system.services.*` actually loads the
  referenced files. Once live, the forward-decl stubs go away and
  `composition Services` in `services/system/system.art` becomes the
  real aggregator (not a stub-list).
- `docs/tasks/TODO/autosar-regen-package-names.md` — update
  `artheia gen-autosar-system`, `gen-can-codec`, `gen-fibex-codec`
  to emit packages under `system.autosar.mlbevo_gen2.{kcan,fibex}`
  and write to the new directory layout. (The 164KB top-level
  `system.art` is still in the old package since it's regenerator
  output.)

## Known regression

The manifest pipeline (`artheia executor emit`) is broken in this
state because the per-FC symlinks at `platform/system/<short>/` are
gone (consolidated into one `services` aggregator). The artheia
manifest loader still uses `art_root/<short>/package.art` lookup with
`art_root=platform/system`. Fix lives in the cross-file-imports
follow-up: the manifest loader will switch to FQN-based import
resolution rather than walking the filesystem directly.
# theia skills refresher — DONE (2026-05-27)

Restructured the single stale `docs/skills/artheia/SKILL.md` into a
`docs/skills/theia/` skill tree: a top-level orientation refresher for new
agents + three reference subskills loaded on demand.

```
docs/skills/theia/
├── SKILL.md                          ← orientation: abstraction ladder,
│                                       repo map, FC catalog, runtime,
│                                       supervisor, pipeline, build cheat
│                                       sheet, conventions
└── references/
    ├── art-lang-grammar.md           ← the .art DSL (grammar + examples)
    ├── artheia-gen-app.md            ← gen-app --kind fc: lib/main/impl
    │                                   split, base-class selection,
    │                                   regen-stability guard
    └── artheia-gen-system.md         ← .art → manifest-proto (py) → JSON
                                        manifests → bazel build/dist →
                                        provision; plus AUTOSAR PSP chain
```

The build-system, provision-orchestrate, and AUTOSAR areas from the
original note are folded as sections inside `artheia-gen-system.md` (one
coherent pipeline doc) rather than separate files.

Retired the old `docs/skills/artheia/SKILL.md` (its content was stale —
referenced `platform/system/`, `executor.yaml`, the pre-//system layout).

All facts verified against the current tree (grammar `.tx`, gen-app
`fc_app.py`, the manifest loader/`clusters.py`, and `artheia <cmd> --help`).
Internal links resolve.

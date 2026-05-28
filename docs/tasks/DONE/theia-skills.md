# theia-skills: rename + split references

Done.

## What landed

1. **`docs/skills/artheia/` → `docs/skills/theia/`** — already in
   commit `dd63f98` (refresher trim) + `be7abe2` (lean SKILL.md).
2. **References split** — `artheia-gen-system.md` covered four
   stages (manifest python, JSON serialization, Bazel, provisioning)
   plus the PSP chain. Split into:

   ```
   theia/references/
   ├─ art-lang-grammar.md          (existing)
   ├─ artheia-gen-app.md           (existing)
   ├─ artheia-gen-system.md        TRIMMED — §1 manifest py + §2 JSON
   │                               + pointers
   ├─ build-system.md              NEW    — Bazel rig extension, FC
   │                               daemons, .ipk, regen-stability
   ├─ provision-orchestrate.md     NEW    — Puppet (prov vs orch),
   │                               docker compose dev rig, Pi 4 push
   └─ autosar.md                   NEW    — DBC/FIBEX → catalog →
                                   .art → netgraph; codec/routing
                                   generator surface
   ```

3. **`SKILL.md` table** grown from 3 → 6 rows.

## Reality fixes folded in while splitting

- PSP path was wrong in the old doc: `autosar/mlbevo_gen2_cmp_psp`
  → corrected to `vendor/autosar/mlbevo_gen2_cmp_psp` (matches the
  artheia CLI's `--package vendor.autosar` default).
- Bazel rig repo targets were stale: doc said `executor_json` /
  `executor_json_central`; the real names are
  `@rig_demo//<machine>:image`, `…:executor`, `…:components`,
  plus top-level `executor_yaml` + `machines_yaml` (and `:all`).
- Puppet was described as "package-install body still a stub" —
  no longer true. `deploy/puppet/{provisioning,orchestration}.pp`
  are real, with a clear blast-radius split (full update vs.
  day-to-day app push).
- Docker compose dev rig was missing — added with the
  end-to-end quick-start from `deploy/README.md`.

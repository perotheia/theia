# AUTOSAR regenerator: emit new package layout

## Background

`artheia gen-autosar-system`, `artheia gen-can-codec`, and
`artheia gen-fibex-codec` write `.art` files under
`autosar/mlbevo_gen2_cmp_psp/system/`. Until 2026-05-22 the emitted
shape was:

```
autosar/mlbevo_gen2_cmp_psp/system/
├── system.art                      # package autosar.mlbevo_gen2_cmp_psp.system
├── kcan/package.art                # package autosar.mlbevo_gen2_cmp_psp.system.kcan
└── mlbevo_gen2/package.art         # package autosar.mlbevo_gen2_cmp_psp.system.mlbevo_gen2  ← misnamed (it's a FIBEX export)
```

Manually restructured to:

```
autosar/mlbevo_gen2_cmp_psp/system/
├── system.art                                       # package autosar.mlbevo_gen2_cmp_psp.system  ← regenerator output, unchanged
└── mlbevo_gen2/
    ├── kcan/package.art                             # package system.autosar.mlbevo_gen2.kcan
    └── fibex/package.art                            # package system.autosar.mlbevo_gen2.fibex
```

The top-level `system.art` is still in the old shape because it gets
clobbered by `gen-autosar-system`. The two leaf `package.art` files
(kcan, fibex) were renamed by hand.

## What needs doing

1. **Update `artheia gen-autosar-system`** to emit:
   - `system.art` → `package system.autosar.mlbevo_gen2` (instead of
     `autosar.mlbevo_gen2_cmp_psp.system`).
   - Place it at `autosar/mlbevo_gen2_cmp_psp/system/mlbevo_gen2/system.art`
     (or keep it at the current path — TBD; the regenerator needs to
     know where to write so the file ends up at the directory matching
     its package name).

2. **Update `artheia gen-can-codec`** (for kcan) to emit:
   - `package system.autosar.mlbevo_gen2.kcan`
   - Path: `autosar/mlbevo_gen2_cmp_psp/system/mlbevo_gen2/kcan/package.art`

3. **Update `artheia gen-fibex-codec`** to emit:
   - `package system.autosar.mlbevo_gen2.fibex` (NOT `mlbevo_gen2` —
     that was the misnamed dir from before).
   - Path: `autosar/mlbevo_gen2_cmp_psp/system/mlbevo_gen2/fibex/package.art`

4. **Update the `--package-name` CLI help** in `artheia/artheia/cli.py:283`
   from the stale example `autosar.mlbevo_gen2_cmp_psp.system`.

5. **Sanity-run** the three generators end-to-end against a vendor
   DBC + FIBEX export and confirm they overwrite the expected files
   in the new structure without disturbing the rest.

## What's already in place

- Directory restructure done (`git mv` inside the autosar subtree).
- Two leaf `package.art` files have correct `system.autosar.mlbevo_gen2.{kcan,fibex}`
  package decls.
- Old top-level `system.art` still declares
  `autosar.mlbevo_gen2_cmp_psp.system` — left as-is because the next
  regenerator run will overwrite it.

## Downstream blast radius (check before regenerating)

These files reference the old `autosar.mlbevo_gen2_cmp_psp` path or
package — audit when changing the regenerator:

- `artheia/artheia/cli.py:283` — `--package-name` help string.
- `artheia/artheia/generators/fibex_to_nanopb.py:17` — comment example.
- `artheia/artheia/generators/netgraph_partition.py:20` — example.
- `artheia/artheia/model/bus_catalog.py:15,37,38` — bus-name LUT (this
  is keyed by SHORT bus names like `mlbevo_gen2_a`, probably fine).
- `artheia/artheia/generators/templates/cpp_app/CMakeLists.txt.j2:33,34`
  — `${BAZEL_BIN}/autosar/mlbevo_gen2_cmp_psp/psp_nanopb_pb` paths.
- `rules/psp.bzl`
- `packaging/BUILD.bazel`
- `vendor/autosar/mlbevo_gen2_cmp_psp/BUILD.bazel`
- Docs: `application.md`, `HANDOFF.md`, `README.md`, `ARCHITECTURE.md`,
  skills `artheia/SKILL.md`, `bazel-build/SKILL.md`.

The directory `autosar/mlbevo_gen2_cmp_psp/` itself is intentionally
NOT being renamed (the user's direction was "rename it in system —
generator destination not tracked").

---

## Audit + outcome (2026-05-25)

Audit before doing the work surfaced that several of the doc's
prescriptions misread the codebase. What actually landed in this
session:

### Items doc was wrong about

- **Steps 2 + 3** (patch `gen-can-codec` / `gen-fibex-codec` to emit
  new package decls): **NO-OP**. Those generators emit `.proto` +
  `.c` codec sources, **not** `.art` files. They have no `--package`
  flag. The leaf `mlbevo_gen2/{kcan,fibex}/package.art` files are
  hand-edited under vendor control, not regen output.
- **Step 1** (patch `gen-autosar-system`): generator is already
  parameter-driven — `autosar_system.py:93` emits
  `f"package {package_name}"` verbatim from `--package`. No code
  change to the generator. The doc's premise that the generator
  hardcoded the wrong package was incorrect.

### Items that genuinely needed fixing

- **CLI help string** at `artheia/artheia/cli.py:1093` (line moved
  from the doc's claimed 283): said
  `"autosar.mlbevo_gen2_cmp_psp.system"`. Updated to
  `"system.autosar.mlbevo_gen2"` with the path placement note.
- **CMakeLists template** `cpp_app/CMakeLists.txt.j2:33-34`: hardcoded
  `${BAZEL_BIN}/autosar/mlbevo_gen2_cmp_psp/psp_nanopb_pb` — wrong
  even before this task (missed the `vendor/` prefix that landed in
  #218). Real bazel-bin path is
  `bazel-bin/vendor/autosar/mlbevo_gen2_cmp_psp/psp_nanopb_pb`.
  Patched.

### Items deferred to vendor repo (separate `git push`)

The `vendor/autosar/mlbevo_gen2_cmp_psp/` sub-repo (skyway GitLab,
its own `vendor-tool-up` branch) still has:

- Old top-level `system/system.art` declaring
  `autosar.mlbevo_gen2_cmp_psp.system` — needs replacement by
  regenerating to `system/mlbevo_gen2/system.art` with
  `--package system.autosar.mlbevo_gen2`.

The regen command (verified to work + idempotent in the Robot test):

```
artheia gen-autosar-system \
  --catalog vendor/autosar/mlbevo_gen2_cmp_psp/system/mlbevo_gen2/kcan/catalog.json \
  --catalog vendor/autosar/mlbevo_gen2_cmp_psp/system/mlbevo_gen2/fibex/catalog.json \
  --out    vendor/autosar/mlbevo_gen2_cmp_psp/system/mlbevo_gen2/system.art \
  --package system.autosar.mlbevo_gen2
```

Tracking that in a follow-up file under `docs/tasks/TODO/` is
overkill — when the next vendor PSP push happens, run the command
above, FF-merge in the vendor repo, push to skyway.

### CI safety net

Added `testing/rf_theia/scenarios/_selftest/autosar_regen/` (4 Robot
test cases, all hermetic, no vendor catalogs needed). Catches:

1. `--package` no longer reflected in emitted `package <name>` line.
2. Generator non-determinism (two runs with identical input differ).
3. Catalog `messages` not flowing through to `interface
   senderReceiver <pdu>_Iface` decls.

Runs in `< 1s` via `robot rf_theia/scenarios/_selftest/autosar_regen/`
from `testing/` with the rf-theia .venv.

Bonus: discovered + fixed pre-existing import-path bug in the
`trace_decoder` selftest (Robot couldn't find
`trace_decoder_lib.TraceDecoderLib` under the canonical run command;
fixed both selftests to use `${CURDIR}/<file>.py` library refs).

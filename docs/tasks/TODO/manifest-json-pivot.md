# Two deploy entry points: dev (rig.py) vs deploy (JSON manifests)

**Decided model (user):** keep TWO deliberate entry points, by purpose — do NOT
funnel everything through JSON.

```
DEV CYCLE  (fast, one command, rig.py coupling stays):
    bazel build @rig_zonal//central:image      # rig.bzl extension runs rig-deps, as today

DEPLOY  (rig.py touched ONCE, then pure JSON):
    theia manifest                              # the SOLE rig entry → dist/manifest/*.json
    theia dist dist/manifest/machines.json      # reads JSON, never touches rig.py
```

Rationale: `theia manifest && bazel build <host>/application.json` is too long to
spell for inner-loop dev — so devs keep `bazel build @rig_zonal//...` (rig.py
direct). Deploy goes through JSON because the artifacts must be reproducible +
puppet/cross-arch consume them.

## Locked answers

1. **`theia dist <path>`** takes the **machines.json PATH**. If it's missing or
   stale → **fail loudly** ("run `theia manifest` first"). No silent auto-run.
2. **Arch is in machine.json** (`hardware.cpu.architecture`: x86_64 / aarch64).
   `theia dist` reads it and maps → bazel platform:
       x86_64  → //rules/config:host
       aarch64 → //rules/config:rpi4
   (The dev `bazel build` path keeps its own arch handling via the rig extension.)
4. **`theia manifest` = the sole rig entry for deploy.** Dev `bazel build rig.py`
   stays as the other legitimate entry. rig.bzl extension is NOT severed.

## What is and isn't changing

| | Touches rig.py? | Stays? |
| --- | --- | --- |
| `bazel build @rig_zonal//<m>:image` (dev) | yes (rig.bzl ext) | **YES — unchanged** |
| `rules/rig.bzl` module extension | yes | **YES — unchanged** (dev path) |
| `theia manifest` (NEW) | yes — the one place | new verb |
| `theia dist <machines.json>` | **no** — JSON only | rewritten |
| `theia install` | (later) JSON pivot | step 4 |

The JSON manifests are self-sufficient (verified): `application.json` carries
`components[].bazel_target` + `bazel_buildable`; `machine.json` carries arch +
`opkg_artifacts`; `execution.json` carries `supervisor_tree` (= executor.json).
`pkg_opkg` is a standalone rule (not tied to the extension), takes
`files={label:dest}` + `arch` — exactly the JSON data.

## Plan

### Step 1 — `theia manifest` verb (the sole rig entry)
Extract the `generate-manifest` call into its own verb:
    theia manifest [module] [--rig ATTR] [--out dist/manifest]
Default `demo.manifest.zonal_rig --rig DemoSoftware → dist/manifest`. Emits
`machines.json` + per-host `{machine,application,service,execution}.json`.
`theia dist`/`install` STOP emitting manifests inline.

### Step 2 — `theia dist dist/manifest/machines.json` (JSON-driven)
- Arg = machines.json path. Missing/unreadable → fail with a clear message.
- Read machines.json → host list (kind/manifests_dir).
- Per `kind: target` host:
  - read `<host>/application.json` → buildable `components[].bazel_target`
  - read `<host>/machine.json` → arch → platform label
  - **bazel approach — DECISION NEEDED (A vs B below)** to produce the `.ipk`.
- No `artheia` invocation anywhere in `theia dist`.

#### Bazel approach — DECIDED: (C) genrule parses application.json + packs
A `genrule` (or small rule) that takes **application.json as a srcs input**,
parses it at exec time, and packs the `.ipk`. The bazel_targets live in the
JSON; `theia dist` passes `--platforms` from machine.json.

Bazel constraint resolved: a genrule's deps are fixed at ANALYSIS time, but the
targets are named INSIDE application.json (read at EXEC time). So the genrule's
`srcs` is a **fixed filegroup of ALL buildable binaries** (the 8 FC/demo/
supervisor mains) PLUS application.json. The pack script:
  - reads application.json → `components[].{name, bazel_target, bazel_buildable}`
  - from `$(locations //…:all_binaries)`, picks the ones whose basename matches a
    buildable component `name`
  - copies each to `/opt/theia/bin/<name>` (the derived dest) into the .ipk
    staging tree, writes control (arch from machine.json or the build platform),
    ar+tar.gz → `<host>.ipk`.
A binary present in the filegroup but NOT in this host's application.json is
simply not copied (over-declared dep, harmless). dest is DERIVED
(/opt/theia/bin/<name>) — application.json has no explicit dest, just name.

`--platforms` applies to the whole invocation, so the filegroup cross-compiles
as a unit → ONE `theia dist <host>` per host/arch:
  x86_64  → //rules/config:host
  aarch64 → //rules/config:rpi4

This keeps packing IN bazel (not python), driven by the JSON, with no
generated-BUILD and no rig.py.

### Step 3 — `theia install` JSON pivot (local single-machine)
Read `install/manifest/central/*` instead of `executor emit` + `gen-params`
against the rig. `execution.json.supervisor_tree` replaces `executor.json`;
per-FC params come from application.json/the FC .art (or a params manifest).
Lower priority — local dev path; can lag.

### Non-goals
- Do NOT touch `rules/rig.bzl` / the dev `bazel build @rig_zonal//...` path.
- Do NOT remove `rig-deps`/`executor emit` from artheia — `theia manifest` +
  dev/debug still use them.

## Status

**Steps 1–2 IMPLEMENTED 2026-06-07 (approach C).**
- `theia manifest` — the sole rig entry: `artheia generate-manifest` → JSON +
  drops the bazel glue (per-host `exports_files` BUILD + a top-level BUILD with
  `dist_ipk(name=<host>)` per target machine).
- `rules/dist_ipk.bzl` + `rules/pack_ipk.py` — the per-host `dist_ipk` rule. Its
  `binaries` is a fixed filegroup of ALL buildable mains; pack_ipk.py parses the
  host's application.json, picks the wanted ones, packs `/opt/theia/bin/<name>`
  into an opkg/dpkg-compatible `.ipk` (deterministic; byte-shape matches
  pkg_opkg). Arch from machine.json.
- `theia dist [machines.json]` — JSON-driven, NO rig.py: reads machines.json +
  per-host machine.json (arch → `//rules/config:{host,rpi4}`) and builds
  `//dist/manifest:<host>_ipk --platforms=<label>` once per target host. Missing
  JSON → fails loudly ("run theia manifest first").

Verified: `theia manifest` → JSON+glue; `bazel build //dist/manifest:central_ipk`
→ `Built: central.ipk (amd64, 6 binaries)` at /opt/theia/bin/* mode 0755; full
`theia dist` builds central (amd64) end-to-end from JSON, no rig.py touched.

**Dev path UNCHANGED:** `bazel build @rig_zonal//<host>:image` (rules/rig.bzl).

**Pre-existing gap (NOT this pivot):** compute (aarch64) fails with
`--platforms=//rules/config:rpi4` → "No matching toolchains for
//platform/supervisor/main:supervisor". The aarch64 cc cross-toolchain isn't
registered — `bazel build //platform/supervisor/main:supervisor
--platforms=rpi4` fails identically on the dev path. `theia dist` does the right
thing (derives rpi4, invokes the cross-build) and continues past the failure
(builds what it can, returns non-zero). Separate task: register the aarch64
cc toolchain (rpi4) so cross-compile actually produces arm binaries.

**Step 3 (theia install JSON pivot)** still TODO — lower priority, local path.

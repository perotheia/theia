# Retire PSP arm of gen-app; standalone-app via `--kind lib`

> **DONE (2026-06-04).** S1–S4 complete; the artheia psp arm is gone
> (`cpp_app.py` + `templates/cpp_app/` deleted, `--kind` is now `fc|lib`).
> S5 (odd_path_monitor rewire) and S6 (vendor/odd_path_client drop + doc
> cleanup) are **descoped** — the psp retirement is considered done; the
> remaining odd-path work, if ever resumed, is independent of this task.

## Why

`artheia gen-app --kind psp` ships a separate three-slice cpp_app shape
(LifecycleInterface + GwClient + GwMessageHeader, CMake) intended for
vendor signal-routing apps that linked the gateway in-process. That
approach is being replaced.

**New approach:** a standalone application (e.g. `odd-path-monitor`)
manages its own runnables and lives at `vendor/<app>/`. The artheia
generator emits `<app>/platform/` from `<app>/system/app/component.art`,
where `platform/` carries only `lib/` + `impl/` (no `main/` — the app's
own `main` drives runnables). The application is **self-sufficient
under CMake**: artheia copies the relevant slice of `platform/runtime/`
and the generated `.proto` set into `<app>/platform/` so the app can
rebuild in isolation without the workspace Bazel root. Gateway runs as
a separate Linux service; the app digests gateway CAN/FlexRay over the
standard Theia transport.

## Plan

| step | what | status |
|---|---|---|
| S1 | Add `git@cicd.skyway.porsche.com:PG50/odd-path-monitor.git` to `.repo/local_manifests/`, drop the existing `vendor_odd_path_client.xml`, `repo sync` into `vendor/odd_path_monitor/`. | ✓ done (commit 8a7d546) |
| S2 | Check out the `flexraya_world_source` branch in the new repo; diff against `main` to extract the existing "old approach" gateway-in-process integration as the source of truth for what the new platform layer must replace. | ✓ done (commit 98a209e — see [psp-retirement-s2-integration-map.md](psp-retirement-s2-integration-map.md)) |
| S3 | Back up `vendor/odd_path_monitor/platform/` to `~/up/`, remove it from the vendor working tree, then author `vendor/odd_path_monitor/system/odd_path_monitor/{package,component}.art`, symlink `system/odd_path_monitor` → real spec, and parse-validate. | ✓ done |
| S4 | Extend `artheia gen-app` with `--kind lib` (keep `fc` as default; retire `psp`). `--kind lib` emits `<app>/platform/lib/` + `<app>/platform/impl/` from the `.art` spec **and** copies the runtime+proto slice the app needs so the tree is CMake-buildable standalone. Generated nodes are `GenServer` with signals declared by the application. | ✓ done (artheia commit 6f0a849 on branch gen-app-lib-kind) |
| S5 | Rewire `odd-path-monitor`'s `main` onto the generated `platform/` (replace the in-process gateway integration with TIPC ingress from the gateway service). | |
| S6 | Delete the old `psp` arm: `artheia/artheia/generators/cpp_app.py`, `templates/cpp_app/`, the `psp` branch in `cli.py`. Drop `vendor/odd_path_client/`. Update `docs/skills/theia/references/artheia-gen-app.md` (and the gen-app reference) to document `--kind fc|lib`. | |

## S3 outcome — the spec

`system/odd_path_monitor` → `../vendor/odd_path_monitor/system/odd_path_monitor`
(workspace-level symlink so the FQN `system.odd_path_monitor` is
reachable through the `system/` import root).

`vendor/odd_path_monitor/system/autosar` → `../../../system/autosar`
(mirror the workspace's autosar tree into the vendor's own
`system/` root so `system.autosar.mlbevo_gen2.*` resolves when
parsing is entered via the real vendor path).

The two `.art` files (in the vendor sub-repo, not committed to
pero_theia):

- `system/odd_path_monitor/package.art` — declares
  `node atomic FlexRayIngress` with 18 receiver ports
  (`requires <Pdu>_Iface best_effort` each). The 18 PDU interfaces +
  the PSP mega-node are FORWARD-DECL stubs in the same file
  (same pattern services/com uses); real defs land via
  `import system.autosar.mlbevo_gen2.*` resolution.
- `system/odd_path_monitor/component.art` — declares
  `composition OddPathMonitor` with one prototype (`ingress`).
  Cross-process PSP→app wiring (which `MlbevoGen2_Bus` sender goes
  to which receiver port here) is INTER-PROCESS TIPC and belongs
  at cluster level — deferred to the vendor rig (S5 territory).

Parse-validates clean via `artheia parse system/odd_path_monitor/component.art`.

## S4 outcome — the generator

`artheia gen-app --kind lib <component.art> --out <app>/platform`
produces:

```
<app>/platform/
├── CMakeLists.txt              top-level standalone build
├── lib/                        per-node + FC-wide headers
│   ├── <Node>.hh
│   ├── <Node>_netgraph.hh
│   ├── <fc>_codecs.hh
│   └── Log.hh
├── impl/                       write-once handler stubs
│   └── <Node>_handlers.cc
├── runtime/                    vendored Theia runtime
│   ├── include/                + libgw/include/ for gw_proto.h etc.
│   └── src/                    (6 .cc files: Logger, NodeRef, Timer,
│                                TimerService, TipcMux, Clock)
└── generated/
    ├── platform_runtime/runtime.pb.{c,h}   (nanopb-compiled)
    └── <app-pkg-path>/<leaf>.{proto,pb.c,pb.h}
```

Builds clean on the host: `cmake -S . -B build && cmake --build build -j`
produces three static libs (libplatform_runtime.a,
lib<app>_protos.a, lib<app>_impl.a). The same tree is intended to
cross-build on the RPi4 target — gateway is delivered separately
as a .ipk shipping the libgw .so + headers; this tree links against
that at deploy time.

Node-ownership filter: only nodes referenced as
`prototype <T> <name>` in a composition declared in this .art get
C++ emitted. Forward-decl stubs imported from sibling packages
(e.g. the PSP mega-node referenced to make `requires X_Iface`
resolve) are kept out of the C++ output.

Default `--kind` flipped from `psp` → `fc` (matches the FC-as-primary
direction; `psp` stays available for in-flight migrations).

## S4 known limitations (S5 to address)

- **Empty codec header.** When all 18 messages are imported from the
  PSP (sibling package), `gen-proto-package` only emits messages
  declared in *this* .art — the app's `.proto` is empty, the
  `<fc>_codecs.hh` carries no `DEMO_DECLARE_REMOTE_CODEC` lines, and
  the `protos` static lib is 8 bytes. The forward-decl interfaces in
  package.art declare no `data`. Cross-package message-type wiring
  needs a design decision: either rebind the interfaces in the app's
  package.art (`interface senderReceiver X_Iface { data X record }`,
  with X imported from the PSP), or extend `gen-proto-package` to
  follow `requires <Iface>` cross-refs.
- **AUTO-GENERATED banner** in lib/ files says `--kind fc` because the
  templates are shared with fc_app. Cosmetic; template refactor.

## S3 learnings (worth keeping for later)

1. **textX cross-references are eager** — `requires <Iface>` is
   resolved when textX merges `package.art + component.art`. If the
   interface isn't in the merged source, parse fails BEFORE
   `_resolve_forward_decls` gets a chance to chase `import` lines.
   Cross-package interfaces must be forward-decl'd locally
   (`interface senderReceiver Foo_Iface { }`); the resolver then
   substitutes the real definition from the imported tree.
2. **The import resolver is pure-relative** (no `--root` flag).
   `_import_dir` walks up from the entry file's directory by
   `len(entry_pkg.split('.')) - len(common_prefix)` levels, then
   descends the import remainder. Entering via a workspace symlink
   is fine — `Path.resolve()` follows it to the real path, and the
   relative climb works from there *if* the real directory has the
   FQN tree mirrored (hence `vendor/<app>/system/autosar` symlink).
3. **Cross-package `connect`s aren't a thing in `.art` today.** The
   existing FCs do all wiring inside their own composition; cluster
   `connect` lines are for inter-process wiring at the deployment
   level. PSP→app connects belong in a vendor rig's cluster.art,
   not the app's component.art. Captured here so S4/S5 don't try
   to re-invent it.

## Key constraints (per the user)

- Keep `--kind` flag; allowed values become `fc|lib`, default `fc`.
- Standalone app under `vendor/<app>/platform/` is self-sufficient — runtime + protos vendored in.
- Gateway runs separately as a Linux service; the app does **not** link gateway in.
- `lib` mode emits `GenServer` nodes with application-declared signals; transport is standard Theia/TIPC.
- No `main/` in `platform/`; the app owns its own `main` and runnable lifecycle.

## Open questions (deferred — surface as they come up)

- What's the minimal subset of `platform/runtime/` that `--kind lib` must
  copy? Header-only? With nanopb? With Tracer.hh?
- Does `<app>/platform/` need its own `CMakeLists.txt`, or does the
  app's existing CMake just `add_subdirectory(platform)`?
- Does the app declare its signals in its own `.art`, or import them
  from a `system/services/<fc>/package.art` (re-using FC interface
  decls)?

These get answered in S3–S4 once we see the actual `flexraya_world_source`
diff.

## Side-fix: autosar layout collapse (between S3 and S4)

Two side-by-side `vendor/autosar/{mlbevo_gen2, mlbevo_gen2_cmp_psp}/`
dirs (both checkouts of the same GitLab repo
`PG50/ccstheia/mlbevo_gen2_cmp_psp`, content-identical at HEAD but
the `_cmp_psp/` working tree had an uncommitted regenerated
`system/mlbevo_gen2/system.art` — the AUTOSAR mega-node) caused
confusion: the package decls inside use `mlbevo_gen2` (no
`_cmp_psp` suffix), so the directory should match.

Done:
- Backed up the uncommitted `system.art` to
  `~/up/autosar-mega-node-system.art.backup.<ts>` (preserved the
  regen artifact, which `repo sync` would otherwise have ignored).
- Deleted the stale `vendor/autosar/mlbevo_gen2/` (it was the
  outdated of the two — no mega-node `system.art`, came from an
  earlier checkout before the local-manifest path was changed).
- Renamed `vendor/autosar/mlbevo_gen2_cmp_psp/` →
  `vendor/autosar/mlbevo_gen2/` (preserving the uncommitted
  `system.art` along the way).
- Fixed the comment in `.repo/local_manifests/psp.xml` (the path
  attr already said `mlbevo_gen2` — only the prose was stale).
- Updated `.repo/project.list` to match the new path.
- `repo sync` verified the new layout end-to-end.
- Repointed pero_theia's `system/autosar` symlink:
  `../vendor/autosar/mlbevo_gen2_cmp_psp/system` →
  `../vendor/autosar/mlbevo_gen2/system`.
- Re-validated `artheia parse system/odd_path_monitor/component.art`
  and `artheia parse system/system.art` — both clean.

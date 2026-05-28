# Retire PSP arm of gen-app; standalone-app via `--kind lib`

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
| S1 | Add `git@cicd.skyway.porsche.com:PG50/odd-path-monitor.git` to `.repo/local_manifests/`, drop the existing `vendor_odd_path_client.xml`, `repo sync` into `vendor/odd_path_monitor/`. | in-flight |
| S2 | Check out the `flexraya_world_source` branch in the new repo; diff against `main` to extract the existing "old approach" gateway-in-process integration as the source of truth for what the new platform layer must replace. | |
| S3 | Author `vendor/odd_path_monitor/system/<app>/package.art` + `component.art`, relink into `system/` (symlink under `system/<app>` → real spec). | |
| S4 | Extend `artheia gen-app` with `--kind lib` (keep `fc` as default; retire `psp`). `--kind lib` emits `<app>/platform/lib/` + `<app>/platform/impl/` from the `.art` spec **and** copies the runtime+proto slice the app needs so the tree is CMake-buildable standalone. Generated nodes are `GenServer` with signals declared by the application. | |
| S5 | Rewire `odd-path-monitor`'s `main` onto the generated `platform/` (replace the in-process gateway integration with TIPC ingress from the gateway service). | |
| S6 | Delete the old `psp` arm: `artheia/artheia/generators/cpp_app.py`, `templates/cpp_app/`, the `psp` branch in `cli.py`. Drop `vendor/odd_path_client/`. Update `docs/skills/theia/references/artheia-gen-app.md` (and the gen-app reference) to document `--kind fc|lib`. | |

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

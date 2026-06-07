# Rig `:image` packages only the supervisor, not the FC/demo daemons

The per-machine deploy `.ipk` (`@rig_<n>//<machine>:image`) contains **only**
`/usr/bin/supervisor`. None of the FC daemons (sm, log, per, ucm, shwa) or the
demo binaries (p1/p2/p3) land in it — so a containerized supervisor boots, reads
`executor.json`, then crash-loops on `execvp(/opt/theia/bin/sm): No such file or
directory` for every child (exit 139). Found bringing up `docker compose` +
trying `tdb ps` (2026-06-06).

## Evidence

```
$ ar x central_host.ipk && tar tzf data.tar.gz
./usr/bin/supervisor          # ← that's ALL

$ bazel query 'deps(@rig_zonal//central_host:image)' | grep main:
//platform/supervisor/main:supervisor      # only the supervisor

$ bazel query 'deps(@rig_demo//demo_host:image)' | grep -E 'main:(sm|log|...)'
(empty)                        # even the single-machine rig is empty
```

Both `@rig_demo` and `@rig_zonal` are affected — it is NOT a zonal-vs-demo
split problem.

## Root cause (to confirm)

The components ARE correctly declared buildable:
`artheia/manifest/utils.py::component_for(short)` sets
`bazel_target=//services/<short>/main:<short>` + `bazel_buildable=True`, and the
demo app components similarly. But the per-machine `:image` / `:components`
filegroup that `pkg_opkg` packages is derived from the SwComponents **bound to
that machine** — and that binding is coming through empty for everything except
the supervisor (which is in `_PLATFORM_FABRIC_COMPONENTS` directly).

The likely culprit is the **in-flight structured-DSL migration** in
`demo/manifest/zonal_rig.py` (its header: "being migrated from the legacy Layer
shape to SoftwareSpecification … phase 4 swaps the CLI to walk
SoftwareSpecification directly"). The host_machine binding of the FC/demo
SwComponents isn't reaching the rig extension's per-machine component filegroup.
`artheia rig-deps demo.manifest.zonal_rig` returns only
`machines: ['central_host']` and no populated per-machine component list.

## What "done" looks like

- `bazel query 'deps(@rig_demo//demo_host:image)'` includes
  `//services/{sm,log,per,ucm,shwa}/main:*` and the demo
  `//demo/Demo3Way*/main:demo` binaries.
- The built `.ipk`'s `data.tar.gz` drops them at the path the executor tree's
  `start_cmd` expects (`/opt/theia/bin/<name>` given THEIA_ROOT_DIR=/opt/theia),
  OR the executor `start_cmd` and the package layout are reconciled.
- A containerized supervisor spawns all children without `execvp` failures.

## Notes / gotchas

- The `.ipk` path is `/usr/bin/<name>` today (dpkg default from pkg_opkg) but
  the executor `start_cmd` is `/opt/theia/bin/<name>` (THEIA_ROOT_DIR + bin/).
  Either the package `prefix` must put binaries under `/opt/theia/bin/` or the
  start_cmd resolution must map to `/usr/bin`. The LOCAL `theia install` path
  sidesteps this (puppet copies bazel-bin → install/<machine>/bin/<name>
  directly), which is why local works and the .ipk path doesn't.
- gateway is already dropped from `_PLATFORM_FABRIC_COMPONENTS` (stale FC,
  needs gen-app modernization) — separate.
- com is currently hidden from the build (its gRPC bridge fix is a known
  next-session item); it won't be in the image until re-enabled.

## Status

**MOSTLY FIXED 2026-06-06.** Three root causes found + fixed:

1. **App-name mismatch (the main bug).** `zonal_rig.py::_PlatformAppOverlay`
   was named `platform_app`, but `ServicesSoftware`'s app is `services_app`.
   The `.squash()` merges by same-identity name — so the FC components never
   got `host_machine=central_host`; their `""` resolved to `admin_host`.
   Renamed the overlay to `services_app`. Now central_host gets log/per/sm/
   ucm/shwa/supervisor (was: supervisor only).
2. **Wrong install path.** `pkg_opkg` mapped binaries to `/usr/bin/<name>`,
   but the executor `start_cmd` is `bin/<name>` + THEIA_ROOT_DIR=/opt/theia →
   `/opt/theia/bin/<name>`. Repointed the rig.bzl `files` map to
   `/opt/theia/bin/<name>`.
3. **Lost exec bit.** `opkg.bzl` set mode 0755 only for `/usr/bin|/usr/sbin`;
   `/opt/theia/bin` fell to 0644 → `execvp: Permission denied`. Changed the
   rule to 0755 for any `/bin/` or `/sbin/` path.

Verified: a containerized supervisor now execs all 5 FC daemons
(sm/log/per/ucm/shwa) cleanly from the .ipk — no execvp errors.

**Supervisor packaging RESOLVED 2026-06-07:** decided the supervisor rides in
the per-machine bundle (NOT a standalone supervisor.ipk). Reconciled the
three-way path contradiction → all land at `/opt/theia/bin/supervisor`:
- zonal_rig.py opkg_artifacts target_dir `/opt/theia/supervisor/` → `/opt/theia/bin/`.
- provisioning.pp: the opkg_artifacts loop no longer dpkg-installs a standalone
  supervisor.ipk (it doesn't exist) — it declares the `theia` class (per-machine
  bundle install) + drops the systemd UNIT + setcap at `/opt/theia/bin`.
- run-supervisor.sh prefers the dev bind-mount `/usr/bin/theia-supervisor` but
  falls back to the bundle path `/opt/theia/bin/supervisor`.
Verified: `puppet apply central.pp` with NO supervisor bind-mount installs the
supervisor (+ FCs) from the bundle at `/opt/theia/bin/supervisor`. This is the
Pi path too — it just additionally needs arm64 cross-build + a build-host→Pi
.ipk transport step (see container-tipc-reachability.md / the Pi notes).

**STILL OPEN — executor tree slicing (separate bug):** the `CentralRig`
executor.json still lists `app_sup` children p1/p2/p3, which are
compute_host-bound (correctly NOT in central's .ipk). So central's supervisor
keeps restarting p1/p2/p3 with `execvp: No such file or directory`. The
per-machine supervisor-tree slicer (`build_supervisor_tree(rig, machine=...)`)
must drop children whose host_machine != this machine. Track that here; it's
the last gap before a fully-clean central bring-up. (shwa is also pinned to
central by the rig but the zonal intent was shwa→compute via PTM — the PTM
override isn't taking; minor, central-running-shwa is harmless.)

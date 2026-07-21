# Bazel build of manifests, FCs, and rig bundles

Stage 3 of the [pipeline](artheia-gen-system.md). Once the Python rig is
loadable, Bazel takes over: a **module extension** materializes a synthetic
repo per rig at module-resolution time, and from there `bazel build` emits
per-machine `.ipk` bundles, the supervisor manifests, and the FC daemons
themselves.

All commands assume `PATH="$PWD/.venv/bin:$PATH"` — `.bazelrc` sets
`--action_env=PATH` so artheia is on PATH inside Bazel actions too.
Do **not** commit `MODULE.bazel.lock`.

## The rig module extension

`MODULE.bazel` declares the extension; one `declare(...)` per rig:

```python
rig_ext = use_extension("//rules:rig.bzl", "rig_ext")
rig_ext.declare(name = "rig_apps", rig_module = "apps.manifest.rig")
use_repo(rig_ext, "rig_apps")
```

`rules/rig.bzl` runs `artheia rig-deps apps.manifest.rig` at module-load
time, reads the JSON, and generates BUILD content for the synthetic
`@rig_apps` repo. The per-machine subtree is named after the machine in
the rig (e.g. `central_host`, `compute_host`, `demo_host`):

| target | what it is |
| --- | --- |
| `@rig_apps//<machine>:image` | the `pkg_opkg` `.ipk` for that machine |
| `@rig_apps//<machine>:executor` | per-machine `executor.yaml` (supervisor input) |
| `@rig_apps//<machine>:components` | filegroup of every binary on that machine |
| `@rig_apps//:executor_yaml` | combined whole-rig `executor.yaml` |
| `@rig_apps//:machines_yaml` | GUI manifest (per-machine gRPC endpoints) |
| `@rig_apps//:all` | builds every machine + both top-level yamls |

The synthetic repo references your own `cc_binary` / `py_binary` targets
by the `bazel_target` strings declared on each `SwComponent` in the rig
(e.g. `//services/com/main:com`, `//apps/Demo3WayP1/main:apps`) — those
targets must exist in your `BUILD.bazel` files. `.art` edits invalidate the
rig build because `apps/manifest:rig_sources` (in
`apps/manifest/BUILD.bazel`) depends on `//system:art_sources` +
`//services/manifest:service_sources`.

## FC daemons and firmware

These build directly, independent of any rig:

```sh
# Host (Linux) — every shipped FC + the supervisor
bazel build --config=linux //services/com/main:com //services/sm/main:sm \
  //services/log/main:log //services/per/main:per //services/ucm/main:ucm \
  //services/shwa/main:shwa

# RPi4 (aarch64) cross-compile
bazel build --config=rpi4 //services/...
```

The `.art` filegroup itself is a target too:

```sh
bazel build //system:art_sources    # the artheia parser's source-of-truth
```

## Install layout

`//:install` (`pkg_install` in the root `BUILD.bazel`) lays the host
artifacts into the runtime filesystem shape — `theia/bin/`, `theia/lib/`,
`etc/theia/` — which is what `pkg_opkg` then wraps into `.ipk`. To see
what would ship without producing a bundle:

```sh
bazel build //:install
```

## Regen-stability guard

Committed `lib/main/impl/BUILD` files for every FC must equal what
`artheia gen-fc <fc>` produces today. The selftest enforces
this — run it after any template or `.art` change:

```sh
robot rf_theia/scenarios/_selftest/fc_regen_stability/fc_regen_stability.robot
```

If it fails, the fix is to **re-run gen-fc and commit the regen**, not
to edit the generated files.

## Where to look when it breaks

| symptom | look at |
| --- | --- |
| `rig-deps` fails at module-load time | `artheia rig-deps apps.manifest.rig` in isolation; it's a normal Python import error |
| Target `@rig_apps//<m>:image` missing | machine `bazel_buildable=False` in the rig — `pkg_opkg` is skipped on purpose |
| Stale FC scaffold won't compile | re-run `artheia gen-fc <fc>`; `impl/<Node>_handlers.cc` is yours, everything else is regen |
| `.proto` change not picked up | the `.pb.*` are genrule-derived; `bazel clean --expunge` if the genrule cache lies |

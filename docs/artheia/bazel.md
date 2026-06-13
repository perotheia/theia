# Bazel integration — rig.py drives the deploy bundle

The artheia rig manifest is the source of truth for "what runs
where." Bazel needs to honor that — every component the rig
references should be buildable as a Bazel target, and the
per-machine deploy bundle should be assembled by Bazel from the
right component subset.

This doc covers the `@rig_<name>//` synthetic-repo pattern in
[`//rules:rig.bzl`](../../rules/rig.bzl), the
[`artheia rig-deps`](../../artheia/artheia/cli.py) subcommand it
relies on, and the per-component `bazel_buildable` flag in
[`SwComponent`](../../artheia/artheia/manifest/application.py).

## The shape

```
MODULE.bazel
   │
   ▼   load module extension
//rules:rig.bzl::rig_ext
   │
   ▼   declare(name="rig_apps", rig_module="apps.manifest.rig")
synthetic repo @rig_apps//
   │
   ▼   genrule + sh + pkg_opkg
@rig_apps//demo_host:image       ── deploy.ipk
@rig_apps//demo_host:components  ── filegroup of buildable bins
@rig_apps//:executor_yaml        ── supervisor manifest
@rig_apps//:machines_yaml        ── GUI manifest
@rig_apps//:all                  ── union of the above
```

At MODULE-resolution time, Bazel invokes `artheia rig-deps
<module>` on the rig.py. The output JSON drives synthetic
BUILD.bazel emission — one BUILD per machine, plus a top-level
BUILD with executor/machines yaml targets. The emitted files
reference the real cc_binary / py_binary / sh_binary labels
declared in the user's BUILD.bazel files.

## Setup

1. Install the workspace venv (artheia must be on PATH for the
   module extension):

   ```bash
   cd /path/to/theia
   python3 -m venv .venv
   .venv/bin/pip install -e ./artheia
   export PATH="$PWD/.venv/bin:$PATH"
   ```

2. Declare the rig in `MODULE.bazel`:

   ```python
   rig_ext = use_extension("//rules:rig.bzl", "rig_ext")
   rig_ext.declare(name = "rig_apps", rig_module = "apps.manifest.rig")
   use_repo(rig_ext, "rig_apps")
   ```

3. Mark each SwComponent's `bazel_buildable=True` once its
   `bazel_target` resolves to a real Bazel target:

   ```python
   SwComponent(
       name="demo_p1",
       bazel_target="//demo:p1_main",
       art_node="system.demo/DemoP1Composition",
       bazel_buildable=True,   # << only set when //demo:p1_main exists
   )
   ```

   Components with `bazel_buildable=False` (the default) are
   skipped during opkg packaging — their references stay in the
   executor.yaml so the supervisor knows about them, but Bazel
   doesn't try to build/package them.

## Daily flow

```
# Just the executor manifest
bazel build @rig_apps//:executor_yaml
cat bazel-bin/external/+rig_ext+rig_apps/executor.yaml

# Just the GUI manifest
bazel build @rig_apps//:machines_yaml

# Per-machine .ipk image
bazel build @rig_apps//demo_host:image
ls bazel-bin/external/+rig_ext+rig_apps/demo_host/*.ipk

# Everything
bazel build @rig_apps//:all
```

The .ipk contains `/usr/bin/<name>` for each buildable
SwComponent. `scp` it to the target, `opkg install` it.

## When the rig.py changes

The module extension re-evaluates whenever `MODULE.bazel`
changes, but NOT when `rig.py` content shifts (Bazel doesn't
track Python source-file mtimes for module extensions). If you
edit rig.py and Bazel claims `up-to-date`, force a re-fetch:

```bash
bazel mod tidy
# or
bazel sync --configure
```

## Adding a new component to a rig

1. Add the `SwComponent` to the rig.py (typically inside a
   `SoftwareSpecification.applications` set as part of an
   `Append(ApplicationManifest(... components=[...]))`).

2. Create the corresponding Bazel target in the appropriate
   BUILD.bazel (e.g. `cc_binary(name="foo", ...)` under
   `vendor/myapp/`).

3. Set `bazel_buildable=True` on the SwComponent.

4. `bazel mod tidy && bazel build @rig_<name>//<machine>:image` —
   the new component lands in the .ipk.

## Adding a new rig

1. Write the rig.py (or `artheia gen-rig` to scaffold one — see
   [`gen-rig.md`](gen-rig.md)).

2. Append to `MODULE.bazel`:

   ```python
   rig_ext.declare(name = "rig_<vehicle>", rig_module = "<vendor>.manifest.rig")
   use_repo(rig_ext, "rig_<vehicle>")
   ```

3. `bazel build @rig_<vehicle>//<machine>:image` works.

## Multi-machine rigs

Each machine in the rig gets its own BUILD package under the
synthetic repo:

```
@rig_<name>//<machine_a>:image
@rig_<name>//<machine_a>:components
@rig_<name>//<machine_b>:image
@rig_<name>//<machine_b>:components
```

Components route to machines via `ApplicationManifest.host_machine`.
If a SwComponent lives in an application bound to `machine_a`, it
lands in `@rig//machine_a:image`. The synthetic repo aggregates
all machine images under `@rig//:all`.

## Limits today

- **CMake bridge is missing**: `artheia gen-app-composition` emits
  CMake projects (under `demo/build/p<N>_main`) — those aren't
  wired into Bazel yet. The current `demo/BUILD.bazel` ships
  placeholder genrules that emit shell stubs. Replace each with a
  real `cc_binary` once the gen-app-composition output is bridged
  into Bazel (`rules_foreign_cc::cmake_external` or hand-written
  `cc_binary` rules).

- **FC SwComponents aren't buildable**: the 18 FCs in
  `services/manifest/fc.py` default to `bazel_buildable=False`.
  They live as bash daemons under `theia_runtime/services/<short>/`.
  Bridging them into Bazel is a separate task.

- **Cross-rig sharing**: today each `rig_ext.declare(...)`
  materializes its own copy of every component label.
  Deduplication across rigs would require an aggregator extension.
  Not load-bearing for the demo.

- **Bazel-driven supervisor launch**: there's no `bazel run`
  target that bringup a rig (no `theia run` equivalent inside
  Bazel). Use `./theia.py run apps.manifest.rig` for that. The
  Bazel side is for building artifacts; the workspace launcher
  is for orchestration.

## Internals

The module extension implementation in
[`rules/rig.bzl`](../../rules/rig.bzl) is ~250 lines. Key bits:

- **`_rig_repo_impl`**: a `repository_rule` that runs `artheia
  rig-deps` and `ctx.read`s the JSON. Synthesizes one BUILD.bazel
  per machine and a top-level BUILD.bazel.

- **`_machine_targets`**: per-machine BUILD content. Filters
  components by `bazel_buildable`, emits a `filegroup` + a
  `pkg_opkg` from `//rules:opkg.bzl`. Skipped components are
  noted as a comment in the generated BUILD.

- **`_abs_label`**: rewrites `//services/com`-style labels to
  `@pero_theia//services/com`. Necessary because labels inside
  the synthetic repo resolve relative to that repo, but the
  user wrote them in rig.py expecting the main repo.

- **`rig_ext`**: a `module_extension` with one tag class
  (`declare`) that loops over every declaration in the module
  graph and instantiates a `rig_repo` per rig.

## Troubleshooting

**`Could not find artheia CLI`**
The workspace venv isn't on PATH at MODULE-resolution time.
Run `source .venv/bin/activate` or `export
PATH="$PWD/.venv/bin:$PATH"` BEFORE `bazel build ...`.

**`no such package 'services/diag'`**
Either:
- The component is marked `bazel_buildable=True` but no
  corresponding Bazel package exists. Either create the package
  (e.g. `services/diag/BUILD.bazel`) or flip the flag back to
  `False`.
- The label in `bazel_target` is wrong. Verify against `bazel
  query //services/diag:*`.

**Stale rig.py content**
`bazel mod tidy` re-evaluates module extensions. Module
extensions don't track Python source files for invalidation —
this is a known limitation.

**`pkg_opkg` produces a 0-byte .ipk**
Check that the components listed in `files = {...}` actually
produce file outputs. The opkg rule expects each label to
resolve to exactly one file via `ctx.attr.files`.

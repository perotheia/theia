# Bazel `//platform/manifest:rig` rule — DONE 2026-05-23

## Resolution

Five files landed:

- `rules/manifest.bzl` — new `rig_manifest(name, rig_target, machines,
  srcs)` Starlark macro. Wraps `artheia generate-manifest` in a
  genrule with static outputs (one per machine × 4 manifest kinds +
  `index.yaml`). Emits a top-level filegroup plus per-machine
  filegroups for downstream `srcs = [...]` use.

- `platform/manifest/BUILD.bazel` — calls `rig_manifest` for the
  demo rig with the three machines (admin/central/compute). Lists
  `//demo/manifest:rig_sources` as srcs so any change to the rig's
  inputs forces a re-run.

- `demo/manifest/BUILD.bazel` — exports `rig_sources` (the rig.py
  itself + transitive `services/manifest` + every `.art` file under
  `platform/system/`).

- `services/manifest/BUILD.bazel` + `platform/system/BUILD.bazel` —
  expose `fc_sources` and `art_sources` filegroups for the chain
  above.

## Verification

```
$ bazel build //platform/manifest:rig
Target //platform/manifest:rig up-to-date:
  bazel-bin/platform/manifest/manifest/index.yaml
  bazel-bin/platform/manifest/manifest/admin_host/{machine,application,service,execution}.yaml
  bazel-bin/platform/manifest/manifest/central_host/{machine,application,service,execution}.yaml
  bazel-bin/platform/manifest/manifest/compute_host/{machine,application,service,execution}.yaml
INFO: Elapsed time: 0.794s, Critical Path: 0.28s
```

- No-op re-run: cache-hit only (1 process, internal).
- Edit `demo/system/demo/component.art` → genrule re-runs (2
  processes: 1 internal + 1 local). Confirmed invalidation works
  across the symlink tree under `platform/system/`.

## What's static vs dynamic

Machines are listed explicitly in `platform/manifest/BUILD.bazel`
because Bazel needs to know outputs at analysis time. Reading the
rig.py during BUILD evaluation is discouraged; listing machines
manually is the lighter, hermetic choice. The rig.py is still the
source of truth for content; the BUILD just enumerates membership.

## Not in scope this turn

- A `.tar.gz` distribution rule wrapping the YAML set. The existing
  `rules/rig.bzl` module extension already produces per-machine
  `.ipk`s; pairing that with this rule's YAML set into a single
  Distribution tarball is a follow-up.

---

## Original ticket follows below

Today `artheia generate-manifest apps.manifest.rig --out dist/manifest`
runs by hand. Wire it into Bazel so:

```
bazel build //platform/manifest:rig
```

emits `dist/manifest/<machine>/{machine,application,service,execution}.yaml`
plus `dist/manifest/index.yaml` as build outputs, with full
dependency tracking on `demo/manifest/rig.py` and every imported `.art`.

## Why

- **Reproducibility:** Bazel caches the emit step; the manifest set
  is a fingerprintable artifact, not a "remember to run the CLI".
- **Hermeticity:** the same manifest set lands wherever the rig
  reference lands (CI, dev, packaging-time).
- **Dependency closure:** downstream rules (`//platform/manifest:dist`
  for tarballs, per-machine `pkg_deb` / `pkg_opkg` targets) can
  `srcs = [":rig"]` and inherit the dependency graph automatically.

## Sketch

```python
# rules/manifest.bzl
load("@rules_python//python:defs.bzl", "py_binary")

def rig_manifest(name, rig_target, **kwargs):
    """Emit dist/manifest/ for a vehicle rig.

    Args:
      name: target name (typically "rig").
      rig_target: dotted Python path to the Rig / SoftwareSpecification
                  module (e.g. "apps.manifest.rig").
    """
    native.genrule(
        name = name,
        outs = [
            "manifest/index.yaml",
            # Per-machine files added dynamically: see
            # rig_manifest_per_machine() below for the static-output
            # variant.
        ],
        cmd = """
            mkdir -p $(RULEDIR)/manifest
            $(location //artheia:cli) generate-manifest \\
                {rig_target} \\
                --out $(RULEDIR)/manifest
        """.format(rig_target = rig_target),
        tools = ["//artheia:cli"],
        **kwargs
    )

def rig_distribution(name, manifest, machines, **kwargs):
    """Package each machine's manifest dir + .ipks into a tarball."""
    for m in machines:
        native.pkg_tar(
            name = "{}_{}".format(name, m),
            srcs = ["{}_{}_files".format(manifest, m)],
            extension = "tar.gz",
            **kwargs
        )
```

```python
# platform/manifest/BUILD.bazel
load("//rules:manifest.bzl", "rig_manifest", "rig_distribution")

rig_manifest(
    name = "rig",
    rig_target = "apps.manifest.rig",
)

rig_distribution(
    name = "dist",
    manifest = ":rig",
    machines = ["admin_host", "central_host", "compute_host"],
)
```

## Static vs dynamic outputs

Bazel needs to know outputs at analysis time. Two options:

**A. Read the rig at load time** to learn the machine list. This means
running Python during BUILD-file evaluation, which Bazel discourages.

**B. Declare machines explicitly in the BUILD.bazel** (as in the
sketch above). The rig.py is the source of truth for the YAML
content; the BUILD.bazel just enumerates which machines exist.
Lighter, hermetic.

Go with **B**.

## Definition of done

1. `bazel build //platform/manifest:rig` succeeds and writes
   `bazel-bin/platform/manifest/manifest/<machine>/{machine,application,service,execution}.yaml` + `index.yaml`.
2. Touching `demo/manifest/rig.py` re-triggers the rule.
3. Touching `services/system/log/package.art` re-triggers the rule
   (because the audit step that runs in `generate-manifest` reads
   the .art tree). This may need an explicit `data` glob on the
   rule listing every `.art` under `platform/system/**`.
4. `bazel build //platform/manifest:dist_central_host` produces a
   `.tar.gz` containing that machine's manifest dir + every
   `.ipk` listed in its `application.yaml`.

## Not in scope (defer)

- `.deb` / `.ipk` packaging for the HostMachine — that's already
  partially wired in `rules/rig.bzl`; this task only adds the
  manifest-emit step. The dist rule is a stub here.
- Cross-compile output paths (rpi4 etc.) — independent concern.

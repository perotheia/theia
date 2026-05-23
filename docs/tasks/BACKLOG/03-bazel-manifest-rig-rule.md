# Bazel `//platform/manifest:rig` rule

Today `artheia generate-manifest demo.manifest.rig --out dist/manifest`
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
                  module (e.g. "demo.manifest.rig").
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
    rig_target = "demo.manifest.rig",
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

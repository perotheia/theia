"""manifest.bzl — Bazel macros for the per-machine deploy manifest set.

Wraps ``artheia generate-manifest`` so the YAML files it writes become
fingerprintable build outputs:

    bazel build //platform/manifest:rig

emits each ``dist/manifest/<machine>/{machine,application,service,
execution}.yaml`` plus ``dist/manifest/index.yaml`` under
``bazel-bin/platform/manifest/``.

Downstream rules can ``srcs = ["//platform/manifest:rig"]`` to depend
on the full set, or address per-machine files via the named-output
filegroups (``//platform/manifest:central_host_manifest`` etc).

Why this exists separately from ``rules/rig.bzl``:

- ``rig.bzl`` is a *module extension* — it runs at MODULE.bazel
  resolution time and synthesizes a whole repo (``@rig_demo``) for
  the .ipk packaging path.
- This file is a plain Starlark *macro* — it runs at analysis time
  and emits genrules consumed by user-side BUILD files. Lighter, no
  synthetic repo, no MODULE.bazel touch needed.

Both ultimately call the same ``artheia`` CLI; they're just different
entry points into the same data.
"""

# Static list of YAML files emitted per machine. Kept in sync with
# `artheia/generators/dist_manifest.py:emit_dist_manifest` — when that
# emitter grows a new file kind, add it here too.
_PER_MACHINE_FILES = [
    "machine.yaml",
    "application.yaml",
    "service.yaml",
    "execution.yaml",
]

def rig_manifest(name, rig_target, machines, **kwargs):
    """Emit dist/manifest/ for a vehicle rig.

    Args:
      name: target name (typically ``rig``).
      rig_target: dotted Python path to the Rig / SoftwareSpecification
                  module (e.g. ``"demo.manifest.rig"``).
      machines: list of machine names (matches ``Machine.name`` in
                rig.py). Static here because Bazel needs to know
                outputs at analysis time; the rig.py is the source of
                truth for content, the BUILD.bazel for membership.
      **kwargs: forwarded to the underlying genrule.

    Generates:
      - ``<name>``       — filegroup of every emitted file
      - per-machine: ``<name>_<machine>_files`` filegroup
      - per-file:    ``manifest/<machine>/<file>.yaml`` as raw outputs
                     of an internal genrule

    The genrule invokes ``artheia generate-manifest`` and writes into
    ``$(RULEDIR)/manifest/``. Bazel handles the cache / fingerprint;
    touching the rig.py (or any imported .art via the artheia
    resolver) re-triggers the rule.
    """

    # Build the static output list. Bazel needs every output declared
    # up-front so analysis can wire downstream deps.
    outs = ["manifest/index.yaml"]
    for m in machines:
        for f in _PER_MACHINE_FILES:
            outs.append("manifest/{}/{}".format(m, f))

    # The genrule itself. `artheia` is expected to be on PATH (the
    # workspace .venv/bin convention — see docs/build.md). If absent,
    # Bazel will fail with a clear "command not found" rather than
    # silently miscompiling, which is what we want.
    native.genrule(
        name = "_" + name + "_emit",
        outs = outs,
        cmd = """
            mkdir -p $(RULEDIR)/manifest
            artheia generate-manifest {rig_target} --out $(RULEDIR)/manifest
        """.format(rig_target = rig_target),
        # The rig.py and its transitive .art imports are read by
        # artheia at runtime. Bazel doesn't see those deps natively,
        # so the user passes them via `srcs` (any *.art / *.py the
        # rig pulls in). Touching one of them re-fingerprints the
        # rule and forces a re-run.
        srcs = kwargs.pop("srcs", []),
        local = True,  # artheia talks to .art symlinks; sandboxing flakes
        **kwargs
    )

    # Per-machine filegroup for downstream `srcs = []` use.
    for m in machines:
        native.filegroup(
            name = "{}_{}_manifest".format(name, m),
            srcs = ["manifest/{}/{}".format(m, f) for f in _PER_MACHINE_FILES],
            visibility = ["//visibility:public"],
        )

    # Top-level filegroup covering everything (the usual entry point).
    native.filegroup(
        name = name,
        srcs = ["manifest/index.yaml"] + [
            ":{}_{}_manifest".format(name, m) for m in machines
        ],
        visibility = ["//visibility:public"],
    )

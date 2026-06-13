"""manifest/rig.py — your deploy rig (one machine to start).

The rig is the Python module artheia reads to know which nodes deploy where.
`@rig_myapp` in MODULE.bazel points here; `theia install`/`theia manifest`
materialize it.

This is a STUB — fill it in once your `system/myapp/` has nodes. The shape:
build a `SoftwareSpecification` (or the legacy `Layer`/`Rig`) over your
compositions, listing each process and its machine. See the Theia docs
(`docs/artheia/manifest-dsl.md`, `docs/artheia/gen-rig.md`) and the in-repo
`apps/manifest/rig.py` for a worked example.

    from artheia.manifest.rig import SoftwareSpecification
    # ... define your machine(s), application(s), process(es) ...
    MyAppSoftware = SoftwareSpecification(...)
    MyAppRig = ...  # materialized Rig for the CLI

Until then, you can still scaffold + build your FCs (`artheia gen-app` +
`bazel build //myapp/...`) and run a node standalone with `artheia.probe`.
"""
from __future__ import annotations

# from artheia.manifest.rig import SoftwareSpecification
# MyAppSoftware = SoftwareSpecification(name="myapp", ...)

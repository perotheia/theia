"""Adaptive Platform manifest — GENERATED from services/system/system.art.

Do not edit by hand. Edit the ``cluster`` declarations in the source
``.art`` and regenerate:

    artheia gen-manifest services/system/system.art <this file>

ARA manifest sections (see docs/autosar/manifest.md):

  * Application — one ``<Cluster>_*`` group per ``cluster`` in the .art
                  (SwComponent + Executable + Process per member).
  * Machine     — empty; rig layers (demo/manifest/rig.py) fill it.
  * Service     — ServiceManifest instances (loader-derived in
                  platform.py from the same cluster members).
  * Execution   — Processes (one per cluster member).

Upper layers patch this base by name (:class:`Override`) — see
``demo/manifest/rig.py``.
"""

from __future__ import annotations

# The per-cluster section builders live in artheia.manifest.utils so the
# generated file stays small and the build logic can evolve without
# regenerating. Imported under the leading-underscore names the sections
# below call.
from artheia.manifest.layer import Layer
from artheia.manifest.utils import (
    app_component_for,
    app_process_for,
    component_for as _component_for,
    executable_for as _executable_for,
    process_for as _process_for,
)


# ---------------------------------------------------------------------------
# Application section — cluster `Services`.
# Each member: (ident, composition, [hosted node names]) — all from the .art.
# Build/deploy paths derive from (base_dir='services', ident) via the
# directory convention (artheia.manifest.utils).
# ---------------------------------------------------------------------------
SERVICES_MEMBERS: list[tuple[str, str, list[str]]] = [
    ('com', 'Com', []),
    ('log', 'Log', []),
    ('per', 'Per', []),
    ('sm', 'Sm', []),
    ('ucm', 'Ucm', []),
    ('shwa', 'Shwa', [])
]
SERVICES_SHORTS = [m[0] for m in SERVICES_MEMBERS]
SERVICES_COMPONENTS = [
    app_component_for('services', ident, comp)
    for ident, comp, _ in SERVICES_MEMBERS
]
SERVICES_EXECUTABLES = [_executable_for(ident) for ident, _, _ in SERVICES_MEMBERS]
SERVICES_PROCESSES = [
    app_process_for('services', ident, nodes)
    for ident, _, nodes in SERVICES_MEMBERS
]

# ---------------------------------------------------------------------------
# Aggregate across all clusters (every component / process).
# ---------------------------------------------------------------------------
COMPONENTS = SERVICES_COMPONENTS
EXECUTABLES = SERVICES_EXECUTABLES
PROCESSES = SERVICES_PROCESSES

# ---------------------------------------------------------------------------
# Machine section — EMPTY. Machines are a deploy-time concern; rig layers
# (demo/manifest/rig.py) add MachineManifests. The spec declares none.
# ---------------------------------------------------------------------------
MACHINES: list = []


# ---------------------------------------------------------------------------
# Supervisor tree — SIDECARED in services/manifest/executor.py.
#
# The supervisor hierarchy (restart strategies + child grouping) is
# hand-authored and has NO .art declaration, so it must survive any
# regeneration of THIS file. It lives in the executor.py sidecar; we
# re-export it here so existing consumers keep reading ``SUPERVISORS``
# unchanged. Edit the tree in executor.py.
# ---------------------------------------------------------------------------

from services.manifest.executor import SUPERVISORS  # noqa: E402,F401


# ---------------------------------------------------------------------------
# Per-cluster Layer + SoftwareSpecification. One pair per .art cluster,
# named after the cluster (`<Cluster>Layer` / `<Cluster>Software`). The
# layer's `name=` is the lowercased cluster name. Upper layers (rig.py)
# compose against these — e.g. `DemoSoftware = ApplicationsSoftware.
# squash(DemoSpecLayer)`.
# ---------------------------------------------------------------------------

from typing import cast

from artheia.manifest.application import ApplicationManifest
from artheia.manifest.layer import Layer  # noqa: E402,F811
from artheia.manifest.rig import SoftwareSpecification, VehicleIdentity
from artheia.manifest.transform import Append, SetTransformTypes  # noqa: E402

# cluster `Services` → ServicesLayer / ServicesSoftware.
ServicesLayer = Layer(
    name="services",
    add_components=SERVICES_COMPONENTS,
    add_executions=SERVICES_PROCESSES,
    add_supervisors=SUPERVISORS,
)
_ServicesApp = ApplicationManifest(
    name="services_app",
    host_machine="",  # rig layers fill in
    components=list(SERVICES_COMPONENTS),
)
ServicesSoftware: SoftwareSpecification = SoftwareSpecification(
    vehicle=VehicleIdentity(name=""),  # rig layers override
    applications=cast(set[SetTransformTypes], {
        Append(_ServicesApp),
    }),
    execution_manifests=cast(set[SetTransformTypes], {
        Append(p) for p in SERVICES_PROCESSES
    }),
    supervisors=cast(set[SetTransformTypes], {
        Append(s) for s in SUPERVISORS
    }),
)


__all__ = [
    "SERVICES_MEMBERS",
    "SERVICES_SHORTS",
    "SERVICES_COMPONENTS",
    "SERVICES_EXECUTABLES",
    "SERVICES_PROCESSES",
    "ServicesLayer",
    "ServicesSoftware",
    "MACHINES",
    "COMPONENTS",
    "EXECUTABLES",
    "PROCESSES",
    "SUPERVISORS",
]

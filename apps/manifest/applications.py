"""Adaptive Platform manifest — GENERATED from apps/system/demo/component.art.

Do not edit by hand. Edit the ``cluster`` declarations in the source
``.art`` and regenerate:

    artheia gen-manifest-proto apps/system/demo/component.art <this file>

ARA manifest sections (see docs/autosar/manifest.md):

  * Application — one ``<Cluster>_*`` group per ``cluster`` in the .art
                  (SwComponent + Executable + Process per member).
  * Machine     — empty; rig layers (apps/manifest/rig.py) fill it.
  * Service     — ServiceManifest instances (loader-derived in
                  platform.py from the same cluster members).
  * Execution   — Processes (one per cluster member).

Upper layers patch this base by name (:class:`Override`) — see
``apps/manifest/rig.py``.
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
# Application section — cluster `Applications`.
# Each member: (ident, composition, [hosted node names]) — all from the .art.
# Build/deploy paths derive from (base_dir='apps', ident) via the
# directory convention (artheia.manifest.utils).
# ---------------------------------------------------------------------------
APPLICATIONS_MEMBERS: list[tuple[str, str, list[str]]] = [
    ('p1', 'Demo3WayP1', ['counter', 'driver', 'ticker']),
    ('p2', 'Demo3WayP2', ['observer']),
    ('p3', 'Demo3WayP3', ['incrementer']),
    ('p4', 'Demo3WayP4', ['demo_fsm', 'demo_gate'])
]
APPLICATIONS_SHORTS = [m[0] for m in APPLICATIONS_MEMBERS]
APPLICATIONS_COMPONENTS = [
    app_component_for('apps', ident, comp)
    for ident, comp, _ in APPLICATIONS_MEMBERS
]
APPLICATIONS_EXECUTABLES = [_executable_for(ident) for ident, _, _ in APPLICATIONS_MEMBERS]
APPLICATIONS_PROCESSES = [
    app_process_for('apps', ident, nodes)
    for ident, _, nodes in APPLICATIONS_MEMBERS
]

# ---------------------------------------------------------------------------
# Aggregate across all clusters (every component / process).
# ---------------------------------------------------------------------------
COMPONENTS = APPLICATIONS_COMPONENTS
EXECUTABLES = APPLICATIONS_EXECUTABLES
PROCESSES = APPLICATIONS_PROCESSES

# ---------------------------------------------------------------------------
# Machine section — EMPTY. Machines are a deploy-time concern; rig layers
# (apps/manifest/rig.py) add MachineManifests. The spec declares none.
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

# cluster `Applications` → ApplicationsLayer / ApplicationsSoftware.
ApplicationsLayer = Layer(
    name="applications",
    add_components=APPLICATIONS_COMPONENTS,
    add_executions=APPLICATIONS_PROCESSES,
    add_supervisors=SUPERVISORS,
)
_ApplicationsApp = ApplicationManifest(
    name="applications_app",
    host_machine="",  # rig layers fill in
    components=list(APPLICATIONS_COMPONENTS),
)
ApplicationsSoftware: SoftwareSpecification = SoftwareSpecification(
    vehicle=VehicleIdentity(name=""),  # rig layers override
    applications=cast(set[SetTransformTypes], {
        Append(_ApplicationsApp),
    }),
    execution_manifests=cast(set[SetTransformTypes], {
        Append(p) for p in APPLICATIONS_PROCESSES
    }),
    supervisors=cast(set[SetTransformTypes], {
        Append(s) for s in SUPERVISORS
    }),
)


__all__ = [
    "APPLICATIONS_MEMBERS",
    "APPLICATIONS_SHORTS",
    "APPLICATIONS_COMPONENTS",
    "APPLICATIONS_EXECUTABLES",
    "APPLICATIONS_PROCESSES",
    "ApplicationsLayer",
    "ApplicationsSoftware",
    "MACHINES",
    "COMPONENTS",
    "EXECUTABLES",
    "PROCESSES",
    "SUPERVISORS",
]

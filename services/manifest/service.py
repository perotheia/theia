"""Adaptive Platform manifest — GENERATED from services/system/system.art.

Do not edit by hand. Edit the ``cluster`` declarations in the source
``.art`` and regenerate:

    artheia gen-manifest-proto services/system/system.art <this file>

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

# Import from submodules directly (not from artheia.manifest) so this
# module can be imported by artheia.manifest.platform without creating
# a circular dependency through artheia/manifest/__init__.py.
from artheia.manifest.application import (
    BuildTypeEnum,
    Executable,
    ExecutionStateReportingBehaviorEnum,
    RootSwComponentPrototype,
    SwComponent,
)
from artheia.manifest.execution import (
    Process,
    SchedulingPolicy,
    StartupConfig,
    StateDependentStartupConfig,
    TerminationBehaviorEnum,
)
from artheia.manifest.layer import Layer


def _component_for(short: str) -> SwComponent:
    """One bazel-buildable handle per cluster member."""
    daemon_class = "".join(p.capitalize() for p in short.split("_")) + "Daemon"
    return SwComponent(
        name=short,
        bazel_target=f"//services/{short}",
        owner="platform",
        art_node=f"services.{short}/{daemon_class}",
    )


def _executable_for(short: str) -> Executable:
    """Adaptive Application Manifest Executable entry (§3.18)."""
    daemon_class = "".join(p.capitalize() for p in short.split("_")) + "Daemon"
    return Executable(
        name=short,
        category="PLATFORM_LEVEL",
        build_type=BuildTypeEnum.BUILD_TYPE_RELEASE,
        reporting_behavior=(
            ExecutionStateReportingBehaviorEnum.REPORTING_BEHAVIOR_INDIVIDUAL
        ),
        root_sw_component_prototype=RootSwComponentPrototype(
            name=f"{short}_root",
            application_type=daemon_class,
        ),
    )


def _process_for(short: str) -> Process:
    """Execution Manifest Process (§8.2).

    Tries to import ``PROCESS`` from ``manifest.services.<short>.executor``
    (hand-edited, survives ``artheia gen-app``). Falls back to an empty
    start_cmd for members without an executor.py — the supervisor
    refuses to launch those.
    """
    import importlib

    try:
        mod = importlib.import_module(f"manifest.services.{short}.executor")
    except ImportError:
        mod = None

    if mod is not None and hasattr(mod, "PROCESS"):
        return mod.PROCESS

    return Process(
        name=short,
        executable=short,
        function_cluster_affiliation=short,
        start_cmd=[],
        state_dependent_startup_config=[
            StateDependentStartupConfig(
                function_group_state=["Default.Running"],
                startup_config=StartupConfig(
                    name=f"{short}_startup",
                    scheduling_policy=SchedulingPolicy.SCHED_OTHER,
                    scheduling_priority=0,
                    termination_behavior=(
                        TerminationBehaviorEnum.PROCESS_IS_NOT_SELF_TERMINATING
                    ),
                ),
            ),
        ],
    )


# ---------------------------------------------------------------------------
# Application section — cluster `Services`.
# ---------------------------------------------------------------------------
SERVICES_SHORTS: list[str] = ["com", "log", "per", "sm", "ucm", "shwa"]
SERVICES_COMPONENTS = [_component_for(s) for s in SERVICES_SHORTS]
SERVICES_EXECUTABLES = [_executable_for(s) for s in SERVICES_SHORTS]
SERVICES_PROCESSES = [_process_for(s) for s in SERVICES_SHORTS]

# ---------------------------------------------------------------------------
# Aggregate across all Application clusters (what consumers import).
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
# re-export it here so existing consumers keep reading
# ``service.SUPERVISORS`` unchanged. Edit the tree in executor.py.
# ---------------------------------------------------------------------------

from services.manifest.executor import SUPERVISORS  # noqa: E402,F401


# The Layer instance upper layers compose against (aggregate of all
# clusters). A rig layer (demo/manifest/rig.py) wraps it via
# :func:`merge_layers`; Removals / Overrides reach in by short-name.
FcLayer = Layer(
    name="services.fc",
    add_components=COMPONENTS,
    add_executions=PROCESSES,
    add_supervisors=SUPERVISORS,
)


# ---------------------------------------------------------------------------
# Structured-DSL counterpart — :data:`FcSoftware`.
# ---------------------------------------------------------------------------

from typing import cast

from artheia.manifest.application import ApplicationManifest
from artheia.manifest.rig import SoftwareSpecification, VehicleIdentity
from artheia.manifest.transform import Append, SetTransformTypes  # noqa: E402

_PlatformApplication = ApplicationManifest(
    name="platform_app",
    host_machine="",  # rig layers fill in
    components=list(COMPONENTS),
)

FcSoftware: SoftwareSpecification = SoftwareSpecification(
    vehicle=VehicleIdentity(name=""),  # rig layers override
    applications=cast(set[SetTransformTypes], {
        Append(_PlatformApplication),
    }),
    execution_manifests=cast(set[SetTransformTypes], {
        Append(p) for p in PROCESSES
    }),
    supervisors=cast(set[SetTransformTypes], {
        Append(s) for s in SUPERVISORS
    }),
)


__all__ = [
    "SERVICES_SHORTS",
    "SERVICES_COMPONENTS",
    "SERVICES_EXECUTABLES",
    "SERVICES_PROCESSES",
    "MACHINES",
    "COMPONENTS",
    "EXECUTABLES",
    "PROCESSES",
    "SUPERVISORS",
    "FcLayer",
    "FcSoftware",
]

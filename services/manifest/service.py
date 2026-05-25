"""Explicit Functional Cluster manifest.

One Python expression per FC, replacing the .art-scanning synthesis that
:data:`artheia.manifest.PlatformBase` used to do. Each FC contributes:

- One :class:`SwComponent` — buildable handle (bazel target + .art ref).
- One :class:`Executable` — Adaptive Application Manifest §3.18 entry.
- One :class:`Process` (Execution Manifest, §8.2) — POSIX-process binding
  with a default :class:`StartupConfig` under ``Default.Running``.

Upper layers patch this base by name (:class:`Override`) — see
``demo/manifest/rig.py`` for an example.
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
from artheia.manifest.clusters import CLUSTERS
from artheia.manifest.execution import (
    Process,
    SchedulingPolicy,
    StartupConfig,
    StateDependentStartupConfig,
    TerminationBehaviorEnum,
)
from artheia.manifest.layer import Layer

# The supervisor tree (SUPERVISORS) is sidecared in executor.py and
# re-exported below — see the "Supervisor tree" section.


def _component_for(short: str) -> SwComponent:
    """One bazel-buildable handle per FC.

    ``bazel_target`` points at where the FC's source/build files would
    live once they're real. Platform-fabric components like the
    supervisor and the gateway service live under ``platform/`` (not
    ``services/``); FCs themselves keep the ``//services/<short>`` label
    layout for source-tree purposes. The .art declaration lives at
    ``platform/system/<short>/package.art`` (symlinked from
    ``services/system/<short>/package.art``). The package path is
    ``services.<short>`` regardless of the filesystem layout —
    package paths and source-tree paths are independent.
    """
    daemon_class = "".join(p.capitalize() for p in short.split("_")) + "Daemon"
    return SwComponent(
        name=short,
        bazel_target=f"//services/{short}",
        owner="platform",
        art_node=f"services.{short}/{daemon_class}",
    )


def _executable_for(short: str) -> Executable:
    """Adaptive Application Manifest Executable entry per FC (§3.18)."""
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


# Per-FC start_cmd. One entry per FC that has a built binary; missing
# entries mean "FC is .art-only" and the supervisor refuses to launch
# them (artheia.manifest.supervisor._fc_child emits an empty start_cmd
# + warning). Setting an entry here makes the FC supervised in the
# dev tree; the install-time .ipk / .deb mapping rewrites these paths
# to the on-target install location.
def _process_for(short: str) -> Process:
    """Execution Manifest Process per FC (§8.2).

    Tries to import ``PROCESS`` from ``manifest.services.<short>.executor``
    — that file is HAND-EDITED and survives every ``artheia gen-app``
    run. Living near the SwComponent + Executable metadata it's the
    rig integrator's single source of truth for start_cmd, scheduling,
    and supervision policy.

    For FCs without an executor.py (the .art-only placeholders), we
    fall back to a Process with an empty start_cmd — the documented
    "no binary built" signal in artheia/manifest/execution.py. The
    supervisor's child entry refuses to launch them; that's correct
    for FCs that don't yet have an implementation (and for `exec`,
    which is implemented by platform/supervisor itself).
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


# Components, executables, processes — one entry per FC, in CLUSTERS order.
COMPONENTS: list[SwComponent] = [_component_for(fc.short) for fc in CLUSTERS]
EXECUTABLES: list[Executable] = [_executable_for(fc.short) for fc in CLUSTERS]
PROCESSES: list[Process] = [_process_for(fc.short) for fc in CLUSTERS]


# ---------------------------------------------------------------------------
# Supervisor tree — SIDECARED in services/manifest/executor.py.
#
# The supervisor hierarchy (restart strategies + child grouping) is
# hand-authored and has NO .art declaration, so it must survive any
# regeneration of THIS file from system.art (the FC list). It therefore
# lives in the executor.py sidecar; we re-export it here so existing
# consumers keep reading ``service.SUPERVISORS`` unchanged. Edit the
# tree in executor.py, not here.
# ---------------------------------------------------------------------------

from services.manifest.executor import SUPERVISORS  # noqa: E402,F401


# The Layer instance upper layers compose against.
#
# This layer ADDs all 18 FCs onto an empty base. A rig layer (e.g.
# ``demo/manifest/rig.py``) wraps it with its own components / process
# mappings via :func:`merge_layers`. Removals and Overrides reach into
# this layer's components/executions by short-name identity.
FcLayer = Layer(
    name="services.fc",
    add_components=COMPONENTS,
    add_executions=PROCESSES,
    add_supervisors=SUPERVISORS,
)


# ---------------------------------------------------------------------------
# Structured-DSL counterpart — :data:`FcSoftware` (mosaic-style).
#
# Same 18 FCs + supervisor tree as ``FcLayer`` above, but in the new
# :class:`SoftwareSpecification` shape: set-typed fields with
# :class:`Append` transforms inline. Built INCREMENTALLY today —
# vehicle layers (e.g. ``DemoSoftware``) compose via
# ``FcSoftware.squash(DemoLayer)``. The legacy ``FcLayer`` /
# ``merge_layers`` route stays functional during the migration.
#
# Eventually ``FcLayer`` and the parallel ``COMPONENTS`` / ``PROCESSES``
# / ``SUPERVISORS`` lists go away once every consumer reads
# ``FcSoftware`` directly. Right now ``artheia.manifest.platform``
# still imports the lists.
# ---------------------------------------------------------------------------

from typing import cast

# Import from submodules directly (same pattern as the imports at the
# top of this file). artheia.manifest.__init__ has a circular dep with
# this module via platform.py — avoid triggering it.
from artheia.manifest.application import ApplicationManifest
from artheia.manifest.rig import SoftwareSpecification, VehicleIdentity
from artheia.manifest.transform import Append, SetTransformTypes  # noqa: E402

# One ``ApplicationManifest`` bagging every SwComponent. Rig layers
# refine: typically Append(ApplicationManifest(name="platform_app",
# host_machine=<rig_host>, components=[...rig_components])) to bind to a
# specific host AND add per-rig binaries. Identity is the name, so
# Append-with-same-name merges via Layer.squash.
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
    "COMPONENTS",
    "EXECUTABLES",
    "PROCESSES",
    "SUPERVISORS",
    "FcLayer",
    "FcSoftware",
]

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
from artheia.manifest.supervisor import (
    AUTO_APPS_CHILDREN,
    RestartStrategy,
    SupervisorNode,
)


def _component_for(short: str) -> SwComponent:
    """One bazel-buildable handle per FC.

    ``bazel_target`` points at where the FC's source/build files would
    live once they're real (today only ``services/supervisor`` and
    ``services/pero_cmp_gw_svc`` are buildable). ``art_node`` follows the
    package declaration inside ``services/system/<short>/package.art``,
    which is ``services.<short>`` (the directory layout under
    ``services/system/`` is filesystem-only and does NOT show up in the
    package path).
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


def _process_for(short: str) -> Process:
    """Execution Manifest Process per FC (§8.2)."""
    return Process(
        name=short,
        executable=short,
        function_cluster_affiliation=short,
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
# Supervisor tree — declarative, hierarchical (see ~/org/1.org).
#
# Each :class:`SupervisorNode` names its children by name. Children
# resolve to either another :class:`SupervisorNode` or a :class:`Process`
# from PROCESSES above. ``AUTO_APPS_CHILDREN`` expands at build time into
# the non-FC SwComponents on the rig (i.e. the vendor/demo applications).
#
# Restart-strategy choices match the OTP design:
#
# - ``root`` is the catastrophic-escalation point. ``one_for_all`` so a
#   top-level escalation reboots the entire subtree.
# - ``ar_sup`` is ``rest_for_one`` — if ``core_sup`` fails, ``app_sup``
#   restarts too (apps can't survive without the platform).
# - ``core_sup`` is ``rest_for_one`` — within the platform layer, ``exec``
#   precedes the others. If it crashes, downstream peers restart in
#   declared order.
# - Per-domain sub-supervisors are ``one_for_one`` (peer-independent).
# - ``app_sup`` is ``one_for_one`` — vendor apps are independent.
# ---------------------------------------------------------------------------

SUPERVISORS: list[SupervisorNode] = [
    SupervisorNode(
        name="root",
        strategy=RestartStrategy.ONE_FOR_ALL,
        children=["ar_sup"],
        tombstone_dir="/tmp/tombstones",
    ),
    SupervisorNode(
        name="ar_sup",
        strategy=RestartStrategy.REST_FOR_ONE,
        children=["core_sup", "app_sup"],
    ),
    SupervisorNode(
        name="core_sup",
        strategy=RestartStrategy.REST_FOR_ONE,
        children=[
            # Leaves first, in declared order — `exec` is the execution
            # manager, so it must come up first. core / crypto / sm
            # follow because the rest_for_one cascade order is meaningful.
            "exec", "core", "crypto", "sm",
            # Per-domain sub-supervisors after the leaves.
            "network_sup", "host_svc_sup", "pltf_sup",
        ],
    ),
    SupervisorNode(
        name="network_sup",
        strategy=RestartStrategy.ONE_FOR_ONE,
        children=["nm", "com", "osi", "idsm", "diag", "tsync"],
    ),
    SupervisorNode(
        name="host_svc_sup",
        strategy=RestartStrategy.ONE_FOR_ONE,
        # "log" is in CLUSTERS for AUTOSAR spec-completeness but has no
        # daemon — logs go to files/console/syslog directly (no FC needed).
        # The trace service lives in services/log/ for convenience, but
        # is a different facility — declared separately.
        children=["per", "rds"],
    ),
    SupervisorNode(
        name="pltf_sup",
        strategy=RestartStrategy.ONE_FOR_ONE,
        # "camera" is listed in ~/org/1.org as an example app — it lands
        # here if a rig layer adds an FC with that short name; otherwise
        # quietly skipped. ``shwa`` and ``fw`` aren't in any branch in
        # ~/org/1.org so they're grouped here as platform-level extras.
        children=["phm", "camera", "ucm", "vucm", "shwa", "fw"],
    ),
    SupervisorNode(
        name="app_sup",
        strategy=RestartStrategy.ONE_FOR_ONE,
        # AUTO_APPS_CHILDREN expands into one leaf per non-FC SwComponent
        # at supervisor-build time (the vendor apps / demo binaries).
        children=[AUTO_APPS_CHILDREN],
    ),
]


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


__all__ = [
    "COMPONENTS",
    "EXECUTABLES",
    "PROCESSES",
    "SUPERVISORS",
    "FcLayer",
]

"""Demo deployment manifest — three-process layout for ``Demo3Way``.

Composition reference (``demo/system/demo/package.art``):

==============  ======================================  ==============
process binary  hosted prototypes (.art)                start_cmd
==============  ======================================  ==============
``demo_p1``     counter_p1, driver_p1, ticker_p1        ``demo/build/p1_main``
``demo_p2``     observer_p2                             ``demo/build/p2_main``
``demo_p3``     incrementer_p3                          ``demo/build/p3_main``
==============  ======================================  ==============

Each process binary boots a :class:`TimerService`, a :class:`TipcMux`,
and the LocalRefs of its hosted prototypes; cross-process traffic
flows through RemoteRefs registered against the mux's listening
TIPC service address.

This file is being migrated from the legacy :class:`Layer` shape
(parallel ``add_machines`` / ``add_components`` / ``add_executions``
lists) to the structured-DSL :class:`SoftwareSpecification` shape
(set-typed fields with inline :class:`Append` / :class:`Remove`
transforms). See ``docs/tasks/PROGRESS/artheia-dsl-recovery.md`` for
the recovery plan.

What's exported:

- ``DemoSoftware`` (NEW) — the :class:`SoftwareSpecification` for the
  rig. Vehicle layers compose against it via ``.squash(...)``.
- ``DemoRig`` (LEGACY) — the materialized :class:`Rig` for the CLI's
  ``artheia executor emit`` / ``artheia gui emit`` (both still
  ``isinstance(x, Rig)``-check on the export).
- ``DemoLayer`` (LEGACY) — the old :class:`Layer`, kept for any
  importer that still needs it.

``DemoRig`` is produced by composing ``PlatformBase.squash(DemoLayer)``
through the legacy path, then ``DemoSoftware.to_rig()`` reconciles to
the same shape. Once the CLI switches to walk
:class:`SoftwareSpecification` directly (phase 4), the legacy exports
go away.
"""

from __future__ import annotations

import dataclasses
from ipaddress import IPv4Address
from typing import cast

from artheia.manifest import (
    ApplicationManifest,
    CpuArchitecture,
    HardwareResource,
    OpkgArtifact,
    OsPackage,
    Layer,
    MachineManifest,
    RestartStrategy,
    Rig,
    SupervisorNode,
    SwComponent,
    VehicleIdentity,
    merge_layers,
)
from artheia.manifest.application import (
    BuildTypeEnum,
    Executable,
    ExecutionStateReportingBehaviorEnum,
    RootSwComponentPrototype,
)
from artheia.manifest.execution import (
    Process,
    SchedulingPolicy,
    StartupConfig,
    StateDependentStartupConfig,
    TerminationBehaviorEnum,
)
from artheia.manifest.machine import CpuResource, IpEndpoint
from artheia.manifest.platform import PlatformBase
from artheia.manifest.rig import SoftwareSpecification
from artheia.manifest.transform import Append, Override, SetTransformTypes
from services.manifest.service import FcSoftware

# ---------------------------------------------------------------------------
# Demo machines.
#
# Two-host topology designed for the Docker-compose deployment under
# `deploy/`:
#
#   central — services (18 FCs) + gateway. The GUI connects here for
#             supervisor introspection.
#   compute — demo binaries (3 process binaries materializing
#             Demo3Way's per-process compositions).
#
# Single-host setups (legacy) can keep collapsing both to one machine
# by overriding the host_machine bindings in their own SoftwareSpec
# layer. The default below is the multi-host shape.
# ---------------------------------------------------------------------------

_PLATFORM_OPKG_ARTIFACTS = [
    OpkgArtifact(
        name="supervisor",
        bazel_target="//platform/supervisor:ipk",
        target_dir="/opt/theia/supervisor/",
        systemd_unit="/etc/systemd/system/theia-supervisor.service",
    ),
    OpkgArtifact(
        name="gateway",
        bazel_target="//platform/gateway:ipk",
        target_dir="/opt/theia/gateway/",
        systemd_unit="/etc/systemd/system/theia-gateway.service",
    ),
]

CentralHost = MachineManifest(
    name="central_host",
    hardware=HardwareResource(
        cpu=CpuResource(architecture=CpuArchitecture.X86_64),
    ),
    # services/com gRPC endpoint — the GUI connects here. In the
    # Docker scenario this is the bridge-IP of the `central` container
    # on the shared network. Loopback default keeps single-host bring-
    # up working.
    com_endpoint=IpEndpoint(
        address=IPv4Address("127.0.0.1"),
        port=7700,
    ),
    # Provisioning: etcd from Ubuntu apt; supervisor + gateway as
    # Theia opkgs under /opt/theia/ with systemd units. Application
    # .ipks (the FCs, the demo binaries) land via the orchestration
    # phase reading application.yaml — not listed here.
    os_packages=[
        OsPackage(name="etcd-server", source="apt"),
        OsPackage(name="libsystemd0", source="apt"),
    ],
    opkg_artifacts=list(_PLATFORM_OPKG_ARTIFACTS),
)

ComputeHost = MachineManifest(
    name="compute_host",
    hardware=HardwareResource(
        cpu=CpuResource(architecture=CpuArchitecture.AARCH64),
    ),
    # The compute container exposes a distinct gRPC port so a multi-
    # machine GUI can talk to both supervisors (one tab per machine).
    com_endpoint=IpEndpoint(
        address=IPv4Address("127.0.0.1"),
        port=7701,
    ),
    os_packages=[
        OsPackage(name="etcd-server", source="opkg"),
        OsPackage(name="libsystemd0", source="opkg"),
    ],
    opkg_artifacts=list(_PLATFORM_OPKG_ARTIFACTS),
)

# Admin console — runs supervisor-gui + supdbg, no supervisor of
# its own. Packaged as a .deb (rules/rig.bzl will branch on
# kind="host"). Its manifest carries the operator's view of the
# rig: every TARGET machine's com_endpoint + etcd_endpoint, plus
# the host's own arch (always x86_64 today — operator laptops are
# all amd64).
#
# At install time the .deb's /etc/theia/machines.yaml is generated
# from this Machine + the rig's TARGET machines so the GUI knows
# what to connect to without further config.
AdminHost = MachineManifest(
    name="admin_host",
    kind="host",   # MachineKind.HOST.value — string-typed; "target" default elsewhere
    hardware=HardwareResource(
        cpu=CpuResource(architecture=CpuArchitecture.X86_64),
    ),
    # No com_endpoint of its own; the GUI talks to TARGET machines.
    # Leave the default — downstream tooling treats a HOST machine's
    # com_endpoint as "n/a".
    #
    # Operator workstation: a Theia .deb is installed via apt, no
    # opkg, no supervisor/gateway. The .deb itself ships the
    # supervisor-gui + supdbg binaries and reads
    # /etc/theia/machines.yaml to find the TARGET endpoints.
    os_packages=[
        OsPackage(name="theia-admin", source="apt"),
    ],
)

# Legacy alias for any caller that still references DemoHost (the
# original single-host name). Points at central by default — the
# services+gateway side is what most existing tests assert against.
DemoHost = CentralHost

# ---------------------------------------------------------------------------
# Process binaries.
# ---------------------------------------------------------------------------

# One SwComponent per process binary built from the Demo3Way composition.
# Today the binaries live at ``demo/build/p{1,2,3}_main`` (see
# ``artheia gen-app-composition``); the bazel_target is the planned
# location for the equivalent ``rules_cc`` target.

_DEMO_PROCESSES = [
    # (process-name, art composition name, bazel target, start_cmd, hosted prototypes)
    #
    # The composition names track demo/system/demo/component.art:
    # `composition Demo3WayP1 { … on process P1 }` materializes into
    # one binary that runs the prototypes listed here in-process.
    #
    # start_cmd is the on-target launch command the supervisor execs for
    # this Process (its app_sup leaf). It points at the built binary
    # — like an FC's Process.start_cmd in services/manifest/executor.py.
    # The install-time .ipk mapping rewrites these to the install path.
    ("demo_p1", "Demo3WayP1",
     "//demo:p1_main", ["demo/build/p1_main"],
     ["counter", "driver", "ticker"]),
    ("demo_p2", "Demo3WayP2",
     "//demo:p2_main", ["demo/build/p2_main"],
     ["observer"]),
    ("demo_p3", "Demo3WayP3",
     "//demo:p3_main", ["demo/build/p3_main"],
     ["incrementer"]),
]

# process-name → start_cmd, for _process_for below.
_DEMO_START_CMD: dict[str, list[str]] = {
    name: start_cmd for (name, _, _, start_cmd, _) in _DEMO_PROCESSES
}

DEMO_COMPONENTS: list[SwComponent] = [
    SwComponent(
        name=name,
        bazel_target=target,
        owner="platform",
        # The art_node points at the top-level composition this process
        # materializes; the runtime constructs its hosted prototypes
        # locally and any other prototype as a RemoteRef.
        art_node=f"system.demo/{art_class}",
        # Demo binaries are real Bazel targets — see demo/BUILD.bazel.
        # The 18 FC SwComponents in services/manifest/service.py default to
        # bazel_buildable=False (they're bash daemons under
        # theia_runtime/, not yet bridged into Bazel).
        bazel_buildable=True,
    )
    for (name, art_class, target, _, _) in _DEMO_PROCESSES
]


# ---------------------------------------------------------------------------
# Platform-fabric SwComponents — these are the daemons referenced by
# `cluster Platform { composition Supervisor sup, composition
# GatewayBridge gw }` in platform/system/system.art. They live alongside
# the FCs (`cluster Services`) and the demo binaries (`cluster
# Demo3Way`) but are functionally distinct: they ARE the platform.
#
# Matches the opkg artifacts declared on each TARGET machine
# (CentralHost / ComputeHost) — same bazel_target stems.
# ---------------------------------------------------------------------------

_PLATFORM_FABRIC_COMPONENTS: list[SwComponent] = [
    SwComponent(
        name="supervisor",
        bazel_target="//platform/supervisor:ipk",
        owner="platform",
        art_node="system.supervisor/Supervisor",
        bazel_buildable=True,
    ),
    SwComponent(
        name="gateway",
        bazel_target="//platform/gateway:ipk",
        owner="platform",
        art_node="system.gateway/GatewayBridge",
        bazel_buildable=True,
    ),
]

# DEMO_BINARIES = just the 3 demo per-process binaries (compute-bound
# AAs). Kept separate from _PLATFORM_FABRIC_COMPONENTS so the
# structured-DSL AAs below can each carry only what belongs to them.
DEMO_BINARIES = list(DEMO_COMPONENTS)

# DEMO_COMPONENTS (legacy-path-only) = demo binaries + platform fabric.
# The legacy merge_layers flow collapses everything into one application
# bag; the structured-DSL flow splits via _ComputeApp / _PlatformAppOverlay
# below — and uses DEMO_BINARIES / _PLATFORM_FABRIC_COMPONENTS directly,
# not this combined list.
DEMO_COMPONENTS = DEMO_BINARIES + _PLATFORM_FABRIC_COMPONENTS


def _executable_for(name: str, art_class: str) -> Executable:
    return Executable(
        name=name,
        category="APPLICATION_LEVEL",
        build_type=BuildTypeEnum.BUILD_TYPE_RELEASE,
        reporting_behavior=(
            ExecutionStateReportingBehaviorEnum.REPORTING_BEHAVIOR_INDIVIDUAL
        ),
        root_sw_component_prototype=RootSwComponentPrototype(
            name=f"{name}_root",
            application_type=art_class,
        ),
    )


DEMO_EXECUTABLES: list[Executable] = [
    _executable_for(name, art_class)
    for (name, art_class, _, _, _) in _DEMO_PROCESSES
]


def _process_for(name: str) -> Process:
    return Process(
        name=name,
        executable=name,
        # Demo processes are application-level, not part of an FC.
        function_cluster_affiliation="",
        # The supervisor execs this when it starts the app_sup leaf —
        # same role as an FC Process.start_cmd. Empty for any process
        # not in the start-cmd map (shouldn't happen for the demo set).
        start_cmd=_DEMO_START_CMD.get(name, []),
        state_dependent_startup_config=[
            StateDependentStartupConfig(
                function_group_state=["Default.Running"],
                startup_config=StartupConfig(
                    name=f"{name}_startup",
                    scheduling_policy=SchedulingPolicy.SCHED_OTHER,
                    scheduling_priority=0,
                    termination_behavior=(
                        TerminationBehaviorEnum.PROCESS_IS_NOT_SELF_TERMINATING
                    ),
                ),
            ),
        ],
    )


DEMO_PROCESSES: list[Process] = [
    _process_for(name) for (name, _, _, _, _) in _DEMO_PROCESSES
]


# ---------------------------------------------------------------------------
# Legacy path — DemoLayer + merge_layers(PlatformBase, [DemoLayer]) → DemoRig.
# Kept until phase 4 swaps the CLI to walk SoftwareSpecification.
# ---------------------------------------------------------------------------

# app_sup ships empty in the platform base (apps belong to the rig).
# This rig attaches its three demo binaries as app_sup's children —
# each resolves through its Process (DEMO_PROCESSES, with a real
# start_cmd) exactly like an FC leaf resolves through its Process.
_APP_SUP_CHILDREN = [name for (name, _, _, _, _) in _DEMO_PROCESSES]

DemoLayer = Layer(
    name="demo",
    set_vehicle=VehicleIdentity(name="demo", make="theia", model="gen_server-demo"),
    add_machines=[DemoHost],
    add_components=DEMO_COMPONENTS,
    add_executions=DEMO_PROCESSES,
    # Override (not Append) so we REPLACE app_sup's empty children list
    # with the demo apps — Override does replace(base, **patch).
    override_supervisors=[
        Override(identity="app_sup", patch={"children": list(_APP_SUP_CHILDREN)}),
    ],
)

DemoRig: Rig = merge_layers(PlatformBase, [DemoLayer])

# Bind the platform application to the demo host. (One application bag
# for the whole rig — services + demo binaries; the supervisor splits
# them into the apps subtree by bazel_target prefix.)
if DemoRig.applications:
    DemoRig.applications[0] = ApplicationManifest(
        name=DemoRig.applications[0].name,
        host_machine=DemoHost.name,
        components=DemoRig.applications[0].components,
    )

# ---------------------------------------------------------------------------
# Pin each FC's ServiceInstance to its host machine.
#
# The artheia FC loader synthesises one ServiceInstance per FC with
# `remote_machine=""`. That empty value triggers the dist_manifest
# emitter's loose-mode fallback (include-everywhere), which is wrong
# in a multi-machine rig — e.g. shwa is compute-node-only because the
# SHWA daemon reads nvidia-smi on the compute box.
#
# Pinning rule for the demo:
#   - shwa → compute_host  (the only compute-pinned FC)
#   - everything else → central_host  (platform + control-plane FCs)
#
# This drives the strict-mode filter in dist_manifest, so
# central_host/service.yaml no longer includes shwa, and
# compute_host/service.yaml contains shwa alone.
# ---------------------------------------------------------------------------

_FC_HOST_MACHINE = {
    "shwa": ComputeHost.name,
}
_DEFAULT_FC_HOST_MACHINE = CentralHost.name

for _sm in DemoRig.service_manifests:
    _sm.instances = [
        dataclasses.replace(
            _i,
            remote_machine=_FC_HOST_MACHINE.get(
                _i.name, _DEFAULT_FC_HOST_MACHINE
            ),
        )
        for _i in _sm.instances
    ]

# ---------------------------------------------------------------------------
# Pin each Process to its host Machine via ProcessToMachineMapping.
#
# AUTOSAR §9.4 PTM is the spec-aligned channel for "this Process runs
# on this Machine." Used by the supervisor-tree slicer in
# `artheia.manifest.supervisor.build_supervisor_tree(rig, machine=)` —
# overrides the AA-host_machine fallback when both signals apply.
#
# Today's rig has zero PTM entries (the slicer falls back to AA
# membership). That's wrong for these three:
#
#   shwa       — AA says central (it's in platform_app), reality is
#                compute (the SHWA daemon reads nvidia-smi there).
#   supervisor — AA says compute (it's in DEMO_COMPONENTS, which
#                landed in compute_app), reality is central. Every
#                TARGET runs its own supervisor binary — the
#                "supervisor" SwComponent here names the .ipk artifact
#                only; central is the canonical host of record.
#   gateway    — same story: .ipk lives in compute_app via
#                DEMO_COMPONENTS, but the gateway daemon binds to
#                central's hardware.
#
# PTM is **sparse-positive** — it lists only deviations from the AA
# default. Unmapped Processes fall through to AA host_machine.
# ---------------------------------------------------------------------------

from artheia.manifest.machine import ProcessToMachineMapping  # noqa: E402

_PROCESS_HOST_OVERRIDES: dict[str, str] = {
    "shwa":       ComputeHost.name,
    "supervisor": CentralHost.name,
    "gateway":    CentralHost.name,
}

_PTM_ENTRIES = [
    ProcessToMachineMapping(
        name=f"ptm_{_proc}",
        process=_proc,
        machine=_mach,
    )
    for _proc, _mach in _PROCESS_HOST_OVERRIDES.items()
]

DemoRig.process_to_machine_mappings = list(
    DemoRig.process_to_machine_mappings
) + _PTM_ENTRIES


# ---------------------------------------------------------------------------
# Structured-DSL path — DemoSoftware = FcSoftware.squash(DemoSpecLayer).
#
# Mirrors the mosaic raj_syscomp.py pattern:
#
#     RajSoftware = MacanSoftware.squash(RajLayer).squash(...)
#
# Here the chain is:
#
#     DemoSoftware = FcSoftware.squash(DemoSpecLayer)
#
# DemoSpecLayer carries the demo-specific deltas (vehicle identity,
# the demo_host machine, the three demo process binaries). FcSoftware
# is the platform-services base. The result is a fully-merged spec
# that .to_rig()s to the same shape as the legacy DemoRig above.
# ---------------------------------------------------------------------------

# Two ApplicationManifests, each bound to its own machine:
#
#   platform_app on central — services (18 FCs from FcSoftware) +
#                              gateway. Same-identity Append merges
#                              into FcSoftware's platform_app: host
#                              binding overrides "" → central_host,
#                              components stay as the 18 FCs (the
#                              demo binaries are routed to compute_app
#                              via the next Append, NOT here).
#   compute_app on compute — the three demo binaries (Demo3Way's
#                             per-process compositions).
#
# This split is what makes `bazel build @rig_demo//central_host:image`
# and `@rig_demo//compute_host:image` produce two distinct .ipks for
# Docker deployment under `deploy/`.

_PlatformAppOverlay = ApplicationManifest(
    name="platform_app",
    host_machine=CentralHost.name,
    # The 18 FC components come from FcSoftware (the platform base);
    # the squash merges them in by same-identity (name="platform_app").
    # We add the platform-fabric components here — supervisor + gateway
    # — because they belong on central and platform_app is its AA.
    components=list(_PLATFORM_FABRIC_COMPONENTS),
)

_ComputeApp = ApplicationManifest(
    name="compute_app",
    host_machine=ComputeHost.name,
    # Compute hosts only the demo per-process binaries. Platform
    # fabric (supervisor / gateway) lives in _PlatformAppOverlay
    # above; FCs (incl. shwa) live in FcSoftware → platform_app, with
    # a PTM entry pinning shwa to compute (see _PTM_ENTRIES).
    components=list(DEMO_BINARIES),
)

DemoSpecLayer = SoftwareSpecification(
    vehicle=VehicleIdentity(name="demo", make="theia", model="gen_server-demo"),
    machines=cast(set[SetTransformTypes], {
        Append(CentralHost),
        Append(ComputeHost),
        Append(AdminHost),
    }),
    applications=cast(set[SetTransformTypes], {
        Append(_PlatformAppOverlay),
        Append(_ComputeApp),
    }),
    execution_manifests=cast(set[SetTransformTypes], {
        Append(p) for p in DEMO_PROCESSES
    }),
    # Carry the pinned service_manifests from the legacy DemoRig
    # (built via merge_layers above) into the structured-DSL output.
    # The artheia loader synthesises one platform_services ServiceManifest
    # with 18 FC instances; we already pinned each instance's
    # remote_machine in the post-merge step above. Append them as a
    # set transform so .squash() picks them up.
    service_manifests=cast(set[SetTransformTypes], {
        Append(_sm) for _sm in DemoRig.service_manifests
    }),
    # Same trick for ProcessToMachineMapping: the PTM overrides we
    # appended to DemoRig above drive the supervisor-tree slicer in
    # `build_supervisor_tree(rig, machine=...)`. Carry them through
    # to the structured-DSL path so DemoSoftware.to_rig() (the one
    # CLI consumers prefer via *Software ranking) gets them too.
    process_to_machine_mappings=cast(set[SetTransformTypes], {
        Append(_ptm) for _ptm in _PTM_ENTRIES
    }),
    # Attach the demo apps to app_sup. FcSoftware ships app_sup with an
    # empty children list; an Append of the same-identity node unions
    # the demo leaves into it (set-transform list-merge), matching the
    # legacy DemoLayer.override_supervisors above. Each child resolves
    # through its DEMO_PROCESSES Process (with a real start_cmd).
    supervisors=cast(set[SetTransformTypes], {
        Append(SupervisorNode(
            name="app_sup",
            strategy=RestartStrategy.ONE_FOR_ONE,
            children=list(_APP_SUP_CHILDREN),
        )),
    }),
)

DemoSoftware: SoftwareSpecification = FcSoftware.squash(DemoSpecLayer)

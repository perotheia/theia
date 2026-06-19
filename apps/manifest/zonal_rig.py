"""Demo deployment manifest — three-process layout for ``Demo3Way``.

Composition reference (``system/apps/package.art``):

==============  ======================================  ==============
process binary  hosted prototypes (.art)                start_cmd
==============  ======================================  ==============
``apps_p1``     counter_p1, driver_p1, ticker_p1        ``apps/build/p1_main``
``apps_p2``     observer_p2                             ``apps/build/p2_main``
``apps_p3``     incrementer_p3                          ``apps/build/p3_main``
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
  rig. Vehicle layers compose against it via ``.mappend(...)``.
- ``DemoRig`` (LEGACY) — the materialized :class:`Rig` for the CLI's
  ``artheia executor emit`` / ``artheia gui emit`` (both still
  ``isinstance(x, Rig)``-check on the export).
- ``DemoLayer`` (LEGACY) — the old :class:`Layer`, kept for any
  importer that still needs it.

``DemoRig`` is produced by composing ``PlatformBase.mappend(DemoLayer)``
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
    Supervisor,
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
from artheia.manifest.applicative import Append, SetTransformTypes
from artheia.manifest.layer import Override
from services.manifest.service import ServicesSoftware

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

# Platform opkg_artifacts. NOTE: the supervisor BINARY is NOT shipped as a
# standalone .ipk — it rides in the per-machine bundle (demo-<machine>.ipk,
# installed at /opt/theia/bin/supervisor by theia::install). These entries
# exist so theia::provisioning can drop the systemd UNIT + enable the service;
# the binary is already on disk from the bundle. target_dir is /opt/theia/bin/
# to match where the bundle lands it (= the executor start_cmd `bin/<name>`
# convention under THEIA_ROOT_DIR=/opt/theia).
_PLATFORM_OPKG_ARTIFACTS = [
    OpkgArtifact(
        name="supervisor",
        bazel_target="//platform/supervisor/main:supervisor",
        target_dir="/opt/theia/bin/",
        systemd_unit="/etc/systemd/system/theia-supervisor.service",
    ),
    # (gateway dropped — stale FC; needs its own gen-app modernization. Re-add
    # once //platform/gateway/main:gateway builds. See gateway-fc-modernization.)
]

CentralHost = MachineManifest(
    name="central",
    # Cluster TIPC instance — central is machine 0. Every node on central binds
    # its stable type at instance=0 (supervisor:0, shwa:0); central com addresses
    # other machines at instance=N over the cluster-wide TIPC fabric.
    machine_index=0,
    # REAL HARDWARE: central = Raspberry Pi 4 (aarch64). The all-x86 docker
    # variant of this 2-machine split lives in apps/manifest/test_rig.py (for
    # provisioning/orchestration testing without arm64 hardware). Building this
    # rig needs the rpi4 aarch64 sysroot + cc-toolchain registered (see
    # third_party/sysroot/setup_rpi4.sh + //rules/config:rpi4).
    hardware=HardwareResource(
        cpu=CpuResource(architecture=CpuArchitecture.AARCH64),
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
        # PTP distribution daemons forked by tsync's prebuilt Providers. central
        # is the GPS-disciplined grandmaster: linuxptp → ptp4l (PTP) + phc2sys
        # (PHC↔system clock). NO gpsd / chrony — the GNSS time source is now an
        # in-process tsync driver (bazel-selectable RTK/NMEA/Fake), and NTP is gone.
        OsPackage(name="linuxptp", source="apt"),
    ],
    opkg_artifacts=list(_PLATFORM_OPKG_ARTIFACTS),
)

ComputeHost = MachineManifest(
    name="compute",
    # Cluster TIPC instance — compute is machine 1. Its supervisor binds
    # supervisor:1, its shwa binds shwa:1; central com reaches them at instance=1.
    machine_index=1,
    hardware=HardwareResource(
        # REAL HARDWARE: compute = NVIDIA Jetson AGX Orin (aarch64) — the GPU/
        # accelerator box (shwa reads nvidia-smi/jtop there). aarch64 like
        # central. Needs the jetson aarch64 sysroot + cc-toolchain registered.
        # (The all-x86 docker variant is apps/manifest/test_rig.py.)
        cpu=CpuResource(architecture=CpuArchitecture.AARCH64),
    ),
    # The compute box exposes a distinct gRPC port so a multi-machine GUI can
    # talk to both supervisors (one tab per machine).
    com_endpoint=IpEndpoint(
        address=IPv4Address("127.0.0.1"),
        port=7701,
    ),
    # etcd is ONE per cluster — it lives on central only (CentralHost above).
    # compute connects to central's etcd; it does not run its own.
    os_packages=[
        OsPackage(name="libsystemd0", source="opkg"),
        # compute is the PTP slave: ptp4l -s syncs FROM central over Ethernet.
        # It needs only linuxptp (ptp4l + phc2sys); no GNSS (no receiver on the
        # accelerator box — its time source is central's PTP) and no chrony/NTP.
        OsPackage(name="linuxptp", source="apt"),
    ],
    opkg_artifacts=list(_PLATFORM_OPKG_ARTIFACTS),
)

# Admin console — runs supervisor-gui + rtdb, no supervisor of
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
    name="admin",
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
    # supervisor-gui + rtdb and reads
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
# Process binaries — DERIVED from the .art, not hand-listed.
# ---------------------------------------------------------------------------
#
# `apps/manifest/applications.py` is GENERATED from `cluster Applications`
# in system/apps/component.art by `artheia gen-manifest`. It
# carries one SwComponent / Executable / Process per cluster member, with
# every path derived from the (base_dir=apps, ident) directory convention
# (app dir apps/<ident>, bazel //apps/<ident>:<ident>, start_cmd bin/<ident>)
# and each Process.nodes set from the composition's hosted prototypes.
#
# rig.py consumes those directly — the hand-written 5-tuple table is gone.
# This module keeps only the deploy concerns: machines, host pinning,
# PTM, and the app_sup override.

from apps.manifest.applications import (  # noqa: E402
    APPLICATIONS_COMPONENTS as _APP_COMPONENTS,
    APPLICATIONS_EXECUTABLES as DEMO_EXECUTABLES,
    APPLICATIONS_PROCESSES as DEMO_PROCESSES,
    APPLICATIONS_SHORTS as _APP_SHORTS,
)


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
        bazel_target="//platform/supervisor/main:supervisor",
        owner="platform",
        art_node="system.supervisor/Supervisor",
        bazel_buildable=True,
    ),
    # (gateway dropped — stale FC; needs its own gen-app modernization. Re-add
    # once //platform/gateway/main:gateway builds. See gateway-fc-modernization.)
]

# DEMO_BINARIES = the demo per-process binaries (compute-bound AAs),
# straight from the generated applications.py. Kept separate from
# _PLATFORM_FABRIC_COMPONENTS so the structured-DSL AAs below can each
# carry only what belongs to them.
DEMO_BINARIES = list(_APP_COMPONENTS)

# DEMO_COMPONENTS (legacy-path-only) = demo binaries + platform fabric.
# The legacy merge_layers flow collapses everything into one application
# bag; the structured-DSL flow splits via _ComputeApp / _PlatformAppOverlay
# below — and uses DEMO_BINARIES / _PLATFORM_FABRIC_COMPONENTS directly,
# not this combined list.
DEMO_COMPONENTS = DEMO_BINARIES + _PLATFORM_FABRIC_COMPONENTS

# DEMO_EXECUTABLES / DEMO_PROCESSES are imported above from
# applications.py — no local builders needed.


# ---------------------------------------------------------------------------
# Legacy path — DemoLayer + merge_layers(PlatformBase, [DemoLayer]) → DemoRig.
# Kept until phase 4 swaps the CLI to walk SoftwareSpecification.
# ---------------------------------------------------------------------------

# app_sup ships empty in the platform base (apps belong to the rig).
# This rig attaches its three demo binaries as app_sup's children —
# each resolves through its Process (DEMO_PROCESSES, with a real
# start_cmd) exactly like an FC leaf resolves through its Process.
_APP_SUP_CHILDREN = list(_APP_SHORTS)

DemoLayer = Layer(
    name="apps",
    set_vehicle=VehicleIdentity(name="apps", make="theia", model="gen_server-apps"),
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

# FCs that run on EVERY target machine (an instance per machine), not pinned to
# one. tsync is the time hierarchy: central = GPS-disciplined PTP grandmaster,
# compute = PTP slave — the SAME FC binary on both, with a per-machine config
# override (deploy/config/<machine>/tsync.json) selecting the GM vs slave profile.
#
# `remote_machine` is single-valued per ServiceInstance, so "runs on both" is
# expressed as TWO instances of the same-named service — one pinned to each
# machine. The strict dist_manifest filter (service.json gets only instances
# whose remote_machine == that machine) then surfaces tsync on BOTH machines'
# service.json; the per-machine supervisor trees (CentralSoftware /
# ComputeSoftware, each built standalone with its OWN children list) list tsync
# under srv_sup on both. See _COMPUTE_FCS / _BOTH_FCS below + the README at
# deploy/config/.
_BOTH_FCS = {"tsync"}

for _sm in DemoRig.service_manifests:
    _pinned = [
        dataclasses.replace(
            _i,
            remote_machine=_FC_HOST_MACHINE.get(
                _i.name, _DEFAULT_FC_HOST_MACHINE
            ),
        )
        for _i in _sm.instances
    ]
    # For every FC that runs on both machines, add a SECOND instance pinned to
    # compute (the default pin above placed the first on central). The two
    # instances differ only by remote_machine (+ instance_id for uniqueness).
    _extra = [
        dataclasses.replace(_i, remote_machine=ComputeHost.name, instance_id=1)
        for _i in _sm.instances
        if _i.name in _BOTH_FCS
    ]
    _sm.instances = _pinned + _extra

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
#   p3         — the one demo app that runs on compute (the per-machine
#                ComputeSoftware partitions it via _COMPUTE_APPS={"p3"};
#                the UNIFIED DemoSoftware reaches the same split only via
#                this PTM, so without it p3 fell through to its AA host
#                and the compute slice was missing p3 entirely).
#
# PTM is **sparse-positive** — it lists only deviations from the AA
# default. Unmapped Processes fall through to AA host_machine.
# ---------------------------------------------------------------------------

from artheia.manifest.machine import ProcessToMachineMapping  # noqa: E402

# Keep in lockstep with the per-machine partition constants below
# (_COMPUTE_FCS={"shwa"}, _COMPUTE_APPS={"p3"}): the unified DemoSoftware
# slicer routes by PTM, the per-machine *Software by list membership — they
# MUST agree on what lands on compute.
_PROCESS_HOST_OVERRIDES: dict[str, str] = {
    "shwa":       ComputeHost.name,
    "p3":         ComputeHost.name,
    "supervisor": CentralHost.name,
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
# Structured-DSL path — DemoSoftware = ServicesSoftware.mappend(DemoSpecLayer).
#
# Mirrors the mosaic raj_syscomp.py pattern:
#
#     RajSoftware = MacanSoftware.mappend(RajLayer).mappend(...)
#
# Here the chain is:
#
#     DemoSoftware = ServicesSoftware.mappend(DemoSpecLayer)
#
# DemoSpecLayer carries the demo-specific deltas (vehicle identity,
# the demo_host machine, the three demo process binaries). ServicesSoftware
# is the platform-services base. The result is a fully-merged spec
# that .to_rig()s to the same shape as the legacy DemoRig above.
# ---------------------------------------------------------------------------

# Two ApplicationManifests, each bound to its own machine:
#
#   platform_app on central — services (18 FCs from ServicesSoftware) +
#                              gateway. Same-identity Append merges
#                              into ServicesSoftware's platform_app: host
#                              binding overrides "" → central_host,
#                              components stay as the 18 FCs (the
#                              demo binaries are routed to compute_app
#                              via the next Append, NOT here).
#   compute_app on compute — the three demo binaries (Demo3Way's
#                             per-process compositions).
#
# This split is what makes `bazel build @rig_apps//central_host:image`
# and `@rig_apps//compute_host:image` produce two distinct .ipks for
# Docker deployment under `deploy/`.

_PlatformAppOverlay = ApplicationManifest(
    # MUST match ServicesSoftware's app name (`services_app`) so the mappend
    # merges by same-identity: that pulls host_machine=central_host onto the
    # FC components (which ship with host_machine="" in the platform base).
    # A mismatched name left the FCs unbound → "" resolved to admin_host, and
    # central_host's .ipk packaged only the supervisor.
    name="services_app",
    host_machine=CentralHost.name,
    # The FC components come from ServicesSoftware (the platform base); the
    # same-identity mappend merges them in. We add the platform-fabric
    # components here — supervisor (+ gateway when it builds) — because they
    # belong on central and services_app is its AA.
    components=list(_PLATFORM_FABRIC_COMPONENTS),
)

_ComputeApp = ApplicationManifest(
    name="compute_app",
    host_machine=ComputeHost.name,
    # Compute hosts the demo per-process binaries PLUS the platform fabric
    # (supervisor) — every machine runs its own supervisor, so the supervisor
    # binary must be in EACH machine's bundle (it's per-machine fabric, not
    # central-only). FCs (incl. shwa) come via ServicesSoftware with a PTM entry
    # pinning shwa to compute (see _PTM_ENTRIES).
    components=list(DEMO_BINARIES) + list(_PLATFORM_FABRIC_COMPONENTS),
)

DemoSpecLayer = SoftwareSpecification(
    vehicle=VehicleIdentity(name="apps", make="theia", model="gen_server-apps"),
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
    # set transform so .mappend() picks them up.
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
    # Attach the demo apps to app_sup. ServicesSoftware ships app_sup with an
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

DemoSoftware: SoftwareSpecification = ServicesSoftware.mappend(DemoSpecLayer)


# ---------------------------------------------------------------------------
# Per-machine split — CentralSoftware + ComputeSoftware.
#
# The demo deploys onto two TARGET machines with DIFFERENT software:
#
#   central_host — the platform services (minus shwa) + the demo apps p1/p2,
#                  under the standard platform supervisor tree.
#   compute_host — shwa (the GPU/accelerator FC) + the demo app p3, under a
#                  small machine-local tree: root → srv_sup → shwa,
#                                              root → app_sup → p3.
#
# We MOVE shwa + p3 to compute by partitioning the manifest lists in plain
# Python — fresh per-machine lists, no Remove/Append transform dance. The
# components/processes are shared dataclasses; we just reference them from
# the right machine's list. Because each machine's SoftwareSpecification
# carries ONLY its own components/processes, the "moved" element simply
# isn't in the other machine's execution tree — the transform semantics
# fall out of set membership, not an explicit Remove.
#
# Two squashes: CentralSoftware = ServicesSoftware.mappend(CentralLayer),
# ComputeSoftware = ServicesSoftware.mappend(ComputeLayer). Each .to_rig()s
# to a single-machine rig the CLI / Bazel emits per host.
# ---------------------------------------------------------------------------

from services.manifest.service import (  # noqa: E402
    SERVICES_COMPONENTS as _FC_COMPONENTS,
    SERVICES_PROCESSES as _FC_PROCESSES,
    SUPERVISORS as _PLATFORM_SUPERVISORS,
)

# What moves off central onto compute.
_COMPUTE_FCS = {"shwa"}      # services moved to compute (compute-ONLY)
_COMPUTE_APPS = {"p3"}       # demo apps moved to compute

# _BOTH_FCS (= {"tsync"}, defined above) run on EVERY machine — they stay on
# central AND ALSO land on compute. Unlike _COMPUTE_FCS (a move/exclusion set),
# these are additive: central keeps them, compute gains them. So the compute FC
# slice is (compute-only ∪ both), while central keeps everything not strictly
# compute-only (which already includes the both-FCs).
_COMPUTE_FCS_ALL = _COMPUTE_FCS | _BOTH_FCS

# --- partition the shared element lists by machine (reference-move) --------
# central: everything not strictly compute-only (so tsync, a _BOTH_FC, stays).
_central_fc_components = [c for c in _FC_COMPONENTS if c.name not in _COMPUTE_FCS]
_central_fc_processes = [p for p in _FC_PROCESSES if p.name not in _COMPUTE_FCS]
# compute: the compute-only FCs PLUS the run-everywhere FCs (tsync).
_compute_fc_components = [c for c in _FC_COMPONENTS if c.name in _COMPUTE_FCS_ALL]
_compute_fc_processes = [p for p in _FC_PROCESSES if p.name in _COMPUTE_FCS_ALL]

_central_app_components = [c for c in DEMO_BINARIES if c.name not in _COMPUTE_APPS]
_central_app_processes = [p for p in DEMO_PROCESSES if p.name not in _COMPUTE_APPS]
_compute_app_components = [c for c in DEMO_BINARIES if c.name in _COMPUTE_APPS]
_compute_app_processes = [p for p in DEMO_PROCESSES if p.name in _COMPUTE_APPS]


def _service_manifests_for(machine: str) -> list:
    """The service_manifests carrying only the ServiceInstances pinned to
    *machine* (remote_machine == machine). The two-instance _BOTH_FCS (tsync)
    surface on BOTH machines this way — each machine keeps its own instance.

    A standalone per-machine spec must carry only its own instances: the
    supervisor-tree slicer resolves a process's host from
    ServiceInstance.remote_machine (first match), so a machine that also held
    the OTHER machine's instance would mis-resolve and drop the FC's leaf.
    """
    out = []
    for _sm in DemoRig.service_manifests:
        local = [_i for _i in _sm.instances if _i.remote_machine == machine]
        out.append(dataclasses.replace(_sm, instances=local))
    return out


_central_service_manifests = _service_manifests_for(CentralHost.name)
_compute_service_manifests = _service_manifests_for(ComputeHost.name)

_central_app_shorts = [c.name for c in _central_app_components]


# --- central supervisor tree: platform tree, app_sup = central apps --------
# Reuse the platform SUPERVISORS as-is (shwa is still *declared* under
# pltf_sup, but with no shwa Process on central its leaf is sliced out at
# build_supervisor_tree time — the honest "declared but not running here").
# Only app_sup needs the central app list.
_central_supervisors = [
    dataclasses.replace(s, children=list(_central_app_shorts))
    if s.name == "app_sup" else s
    for s in _PLATFORM_SUPERVISORS
]

# The UNIFIED (DemoSoftware) tree: app_sup carries EVERY app (p1..p4). The
# per-machine slicer drops each leaf not hosted on the target machine, so
# p1/p2/p4 stay on central and p3 lands on compute — without it, the unified
# tree (which used _central_supervisors) silently omitted p3 everywhere.
_all_app_shorts = [c.name for c in DEMO_BINARIES]
_all_apps_supervisors = [
    dataclasses.replace(s, children=list(_all_app_shorts))
    if s.name == "app_sup" else s
    for s in _PLATFORM_SUPERVISORS
]


# --- compute supervisor tree: fresh, machine-local -------------------------
# root → srv_sup → shwa ; root → app_sup → p3. Small and self-contained;
# nothing of the platform tree applies on the accelerator box.
_compute_supervisors = [
    SupervisorNode(
        name="root",
        strategy=RestartStrategy.ONE_FOR_ALL,
        children=["srv_sup", "app_sup"],
        tombstone_dir="/tmp/tombstones",
    ),
    SupervisorNode(
        name="srv_sup",
        strategy=RestartStrategy.ONE_FOR_ONE,
        # compute-only FCs (shwa) + run-everywhere FCs (tsync, the PTP slave).
        children=sorted(_COMPUTE_FCS_ALL),
    ),
    SupervisorNode(
        name="app_sup",
        strategy=RestartStrategy.ONE_FOR_ONE,
        children=sorted(_COMPUTE_APPS),
    ),
]


def _mk_app(name: str, host: str, components: list) -> ApplicationManifest:
    return ApplicationManifest(
        name=name, host_machine=host, components=list(components)
    )


# Each per-machine spec is built STANDALONE (fresh lists), NOT combined
# onto ServicesSoftware — mappend unions with the base, which would drag the
# full 6-FC set + the platform root tree back in and undo the partition.
# Building fresh means each machine carries exactly its own slice.

# --- CentralSoftware --------------------------------------------------------
CentralSoftware: SoftwareSpecification = SoftwareSpecification(
    vehicle=VehicleIdentity(name="apps", make="theia",
                            model="gen_server-apps"),
    machines=cast(set[SetTransformTypes], {Append(CentralHost)}),
    applications=cast(set[SetTransformTypes], {
        Append(_mk_app("platform_app", CentralHost.name,
                       _central_fc_components + _PLATFORM_FABRIC_COMPONENTS)),
        Append(_mk_app("central_app", CentralHost.name,
                       _central_app_components)),
    }),
    execution_manifests=cast(set[SetTransformTypes], {
        Append(p) for p in (_central_fc_processes + _central_app_processes)
    }),
    service_manifests=cast(set[SetTransformTypes], {
        Append(_sm) for _sm in _central_service_manifests
    }),
    supervisors=cast(set[SetTransformTypes], {
        Append(s) for s in _central_supervisors
    }),
)


# --- ComputeSoftware --------------------------------------------------------
ComputeSoftware: SoftwareSpecification = SoftwareSpecification(
    vehicle=VehicleIdentity(name="apps", make="theia",
                            model="gen_server-apps"),
    machines=cast(set[SetTransformTypes], {Append(ComputeHost)}),
    applications=cast(set[SetTransformTypes], {
        Append(_mk_app("compute_app", ComputeHost.name,
                       _compute_fc_components + _compute_app_components)),
    }),
    execution_manifests=cast(set[SetTransformTypes], {
        Append(p) for p in (_compute_fc_processes + _compute_app_processes)
    }),
    service_manifests=cast(set[SetTransformTypes], {
        Append(_sm) for _sm in _compute_service_manifests
    }),
    supervisors=cast(set[SetTransformTypes], {
        Append(s) for s in _compute_supervisors
    }),
)


# Materialized per-machine rigs — what `artheia executor emit
# apps.manifest.rig --rig CentralRig` (and ComputeRig) consume to write
# install/central/executor.json and install/compute/executor.json.
CentralRig: Rig = CentralSoftware.to_rig()
ComputeRig: Rig = ComputeSoftware.to_rig()


# ---------------------------------------------------------------------------
# DemoSoftware — the deploy spec. The full 2(+admin)-machine vehicle, built
# from the PARTITIONED per-machine apps (NOT the mappend) so each component lands
# on its real machine: shwa + p3 on compute, the rest on central. `theia
# manifest` / `theia dist` emit from this.
#
# (The earlier mappend-based DemoSoftware mis-bound shwa to central — its PTM
# moved the process to compute but not the component. Building from the explicit
# per-machine apps binds the COMPONENT correctly too.)
# ---------------------------------------------------------------------------
DemoSoftware: SoftwareSpecification = SoftwareSpecification(
    vehicle=VehicleIdentity(name="apps", make="theia",
                            model="gen_server-apps"),
    machines=cast(set[SetTransformTypes], {
        Append(CentralHost), Append(ComputeHost), Append(AdminHost),
    }),
    applications=cast(set[SetTransformTypes], {
        # central: platform FCs (minus shwa) + the supervisor fabric + central apps.
        Append(_mk_app("platform_app", CentralHost.name,
                       _central_fc_components + _PLATFORM_FABRIC_COMPONENTS)),
        Append(_mk_app("central_app", CentralHost.name,
                       _central_app_components)),
        # compute: shwa + its supervisor fabric + the compute apps (p3).
        Append(_mk_app("compute_app", ComputeHost.name,
                       _compute_fc_components + _PLATFORM_FABRIC_COMPONENTS
                       + _compute_app_components)),
    }),
    execution_manifests=cast(set[SetTransformTypes], {
        Append(p) for p in (
            _central_fc_processes + _central_app_processes
            + _compute_fc_processes + _compute_app_processes)
    }),
    service_manifests=cast(set[SetTransformTypes], {
        Append(_sm) for _sm in DemoRig.service_manifests
    }),
    supervisors=cast(set[SetTransformTypes], {
        # The UNIFIED tree carries ALL app_sup children (p1..p4); the per-machine
        # slicer (build_supervisor_tree(machine=...)) keeps each leaf only on its
        # host (_process_host via the PTM / AA membership) — p1/p2/p4 on central,
        # p3 on compute. Using _central_supervisors here (app_sup=central apps
        # only, p3 excluded) DROPPED p3 from every machine: central's tree never
        # listed it and compute's tree wasn't represented at all.
        Append(s) for s in _all_apps_supervisors
    }),
    # Per-machine supervisor identity (ARA Executor). Two supervisors on one host
    # TIPC namespace must bind distinct instances: central=0 (default), compute=1
    # — matching the system.art ComputeSupervisor prototype. Flows into
    # <machine>/execution.json.supervisor_instance → THEIA_SUPERVISOR_INSTANCE.
    supervisor={ComputeHost.name: Supervisor(instance=1)},
)


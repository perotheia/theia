"""Demo deployment manifest — three-process layout for ``Demo3Way``.

Composition reference (``demo/system/package.art``):

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

The Layer below ADDs the three process-level SwComponents on top of
:data:`services.manifest.FcLayer` (which carries the 18 FCs). The
resolved :data:`DemoRig` is the input to ``artheia executor emit`` and
``artheia generate-manifest``.
"""

from __future__ import annotations

from artheia.manifest import (
    ApplicationManifest,
    CpuArchitecture,
    HardwareResource,
    Layer,
    MachineManifest,
    Rig,
    SwComponent,
    VehicleIdentity,
    merge_layers,
)
from artheia.manifest.platform import PlatformBase
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
from ipaddress import IPv4Address

from artheia.manifest.machine import CpuResource, IpEndpoint

# ---------------------------------------------------------------------------
# Demo machine — the host (workstation / dev box) the processes run on.
# ---------------------------------------------------------------------------

DemoHost = MachineManifest(
    name="demo_host",
    hardware=HardwareResource(
        cpu=CpuResource(architecture=CpuArchitecture.AARCH64),
    ),
    # Per-machine services/com gRPC endpoint — the GUI connects here.
    # Multi-machine rigs assign distinct ports when several machines
    # share one host. The supervisor itself is *not* exposed directly;
    # it's an in-host TIPC node bridged by com.
    com_endpoint=IpEndpoint(
        address=IPv4Address("127.0.0.1"),
        port=7700,
    ),
)

# ---------------------------------------------------------------------------
# Process binaries.
# ---------------------------------------------------------------------------

# One SwComponent per process binary built from the Demo3Way composition.
# Today the binaries live at ``demo/build/p{1,2,3}_main`` (see
# ``artheia gen-app-composition``); the bazel_target is the planned
# location for the equivalent ``rules_cc`` target.

_DEMO_PROCESSES = [
    ("demo_p1", "DemoP1Composition",
     "//demo:p1_main",
     ["counter_p1", "driver_p1", "ticker_p1"]),
    ("demo_p2", "DemoP2Composition",
     "//demo:p2_main",
     ["observer_p2"]),
    ("demo_p3", "DemoP3Composition",
     "//demo:p3_main",
     ["incrementer_p3"]),
]

DEMO_COMPONENTS: list[SwComponent] = [
    SwComponent(
        name=name,
        bazel_target=target,
        owner="platform",
        # The art_node points at the top-level composition this process
        # materializes; the runtime constructs its hosted prototypes
        # locally and any other prototype as a RemoteRef.
        art_node=f"demo.system/{art_class}",
    )
    for (name, art_class, target, _) in _DEMO_PROCESSES
]


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
    for (name, art_class, _, _) in _DEMO_PROCESSES
]


def _process_for(name: str) -> Process:
    return Process(
        name=name,
        executable=name,
        # Demo processes are application-level, not part of an FC.
        function_cluster_affiliation="",
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
    _process_for(name) for (name, _, _, _) in _DEMO_PROCESSES
]

# ---------------------------------------------------------------------------
# Demo layer — delta on top of services.manifest.FcLayer (already folded
# into PlatformBase via artheia.manifest.platform).
# ---------------------------------------------------------------------------

DemoLayer = Layer(
    name="demo",
    set_vehicle=VehicleIdentity(name="demo", make="theia", model="gen_server-demo"),
    add_machines=[DemoHost],
    add_components=DEMO_COMPONENTS,
    add_executions=DEMO_PROCESSES,
)

# ---------------------------------------------------------------------------
# Final rig — PlatformBase carries the 18 FCs; DemoLayer pins the demo
# binaries on top.
# ---------------------------------------------------------------------------

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

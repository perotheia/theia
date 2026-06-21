"""SPLIT test target — central + compute (the 2-machine deploy).

Services (the GUI-facing FCs) on central; the demo binaries + shwa (safe HW
accelerator) on compute. One base shape; two arch overrides at the bottom:

  DOCKER — both machines x86_64 (the all-x86 docker-compose target)
  HW     — central = Raspberry Pi 4, compute = Jetson AGX Orin (both aarch64)

serialize-manifest manifest.split.rig --attr DOCKER  (or --attr HW)
"""
from __future__ import annotations

from artheia.manifest.algebra import Append, Explicit
from artheia.manifest.deployment import (
    ApplicationLayer,
    ApplicationSetLayer,
    DeploymentLayer,
    ExecutionLayer,
    MachineLayer,
    MachineSetLayer,
    ProcessLayer,
)
from manifest.assemble import (
    BASE,
    BASE_PROCESS_NODES,
    BASE_SUPERVISORS,
    PROCESS_NAMES,
)

# the demo binaries + shwa live on compute; every other process on central.
_ON_COMPUTE = {"p1", "p2", "p3", "p4", "shwa"}
_ON_CENTRAL = [n for n in PROCESS_NAMES if n not in _ON_COMPUTE]

# arch-agnostic split: two machines + every process bound to one of them.
SPLIT = BASE.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={
        MachineLayer(name="central", cores={0, 1, 2, 3}, machine_states={"Startup", "Running"}),
        MachineLayer(name="compute", cores={0, 1, 2, 3, 4, 5, 6, 7}, machine_states={"Startup", "Running"}),
    }),
    execution=ExecutionLayer(processes={
        *(Append(ProcessLayer(name=n, machine=Explicit("central"))) for n in _ON_CENTRAL),
        *(Append(ProcessLayer(name=n, machine=Explicit("compute"))) for n in sorted(_ON_COMPUTE)),
    }),
    applications=ApplicationSetLayer(applications={
        Append(ApplicationLayer(name="services", host_machine=Explicit("central"))),
        Append(ApplicationLayer(name="apps", host_machine=Explicit("compute"))),
    }),
))

SUPERVISORS = BASE_SUPERVISORS
PROCESS_NODES = BASE_PROCESS_NODES

# DOCKER — both machines x86_64 (dev box / CI docker-compose).
DOCKER = SPLIT.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={
        Append(MachineLayer(name="central", arch=Explicit("x86_64"))),
        Append(MachineLayer(name="compute", arch=Explicit("x86_64"))),
    }),
))

# HW — real hardware: central = Raspberry Pi 4, compute = Jetson AGX Orin (aarch64).
HW = SPLIT.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={
        Append(MachineLayer(name="central", arch=Explicit("aarch64"))),
        Append(MachineLayer(name="compute", arch=Explicit("aarch64"))),
    }),
))

# Default RIG attr = DOCKER (the CI target).
RIG = DOCKER

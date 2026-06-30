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

# the demo binaries live on compute; every other process on central. shwa is a
# HOST MONITOR — the SAME logical FC fanned onto BOTH machines via a set-valued
# placement (machines=), not pinned to one. Each board runs its own shwa instance
# (central inst 0, compute inst 1: the supervisor machine-shifts the injected
# --tipc per machine, and shwa stamps its resolved instance as machine_index on
# every AccelSample). Without it the machine that lacks shwa (was: central) shows
# no disk/uptime/load in the GUI/rtdb System view.
_ON_COMPUTE = {"p1", "p2", "p3", "p4"}
_ON_BOTH    = {"shwa"}
_ON_CENTRAL = [n for n in PROCESS_NAMES if n not in _ON_COMPUTE and n not in _ON_BOTH]

# arch-agnostic split: two machines + every process bound to one of them.
SPLIT = BASE.combine(DeploymentLayer(
    # name="split" → the rig identity (machines.json `rig`); the SWP is named from
    # it. NOTE: DOCKER/HW combine onto SPLIT and the --arch/--os rebuild both drop
    # this model name (a default "machine" wins the right-biased fold), so the
    # AUTHORITATIVE source is `theia manifest split` passing --rig-name split; this
    # is the model carrier + documentation.
    machines=MachineSetLayer(name="split", machines={
        # etcd lives on central ONLY (one per cluster, the coordinator); compute
        # connects to it. Provisioning reads machine.json.etcd to place it.
        MachineLayer(name="central", etcd=Explicit(True),
                     cores={0, 1, 2, 3}, machine_states={"Startup", "Running"}),
        MachineLayer(name="compute", cores={0, 1, 2, 3, 4, 5, 6, 7}, machine_states={"Startup", "Running"}),
    }),
    execution=ExecutionLayer(processes={
        *(Append(ProcessLayer(name=n, machine=Explicit("central"))) for n in _ON_CENTRAL),
        *(Append(ProcessLayer(name=n, machine=Explicit("compute"))) for n in sorted(_ON_COMPUTE)),
        # shwa fanned onto BOTH machines (host monitor) via a set placement; the
        # supervisor shifts each board's instance by its machine_index.
        *(Append(ProcessLayer(name=n, machines={"central", "compute"}))
          for n in sorted(_ON_BOTH)),
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

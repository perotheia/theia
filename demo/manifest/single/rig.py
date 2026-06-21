"""SINGLE test target — one machine (central) runs everything.

BASE (services ⊕ apps) with one machine declared and every process bound to it.
The dev-flow rig: `theia manifest` / `serialize-manifest manifest.single.rig`.
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

RIG = BASE.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={
        MachineLayer(name="central", arch=Explicit("x86_64"),
                     cores={0, 1, 2, 3}, machine_states={"Startup", "Running"}),
    }),
    execution=ExecutionLayer(processes={
        Append(ProcessLayer(name=n, machine=Explicit("central"))) for n in PROCESS_NAMES
    }),
    applications=ApplicationSetLayer(applications={
        Append(ApplicationLayer(name="services", host_machine=Explicit("central"))),
        Append(ApplicationLayer(name="apps", host_machine=Explicit("central"))),
    }),
))

SUPERVISORS = BASE_SUPERVISORS
PROCESS_NODES = BASE_PROCESS_NODES

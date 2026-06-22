"""RPI4 test target — one Raspberry Pi 4 (machine `central`) runs everything.

A copy of the SINGLE rig retargeted to the lab rpi4 (rig1-central, 10.0.0.22):
Debian 13 trixie, aarch64, 4 cores. Same one-machine shape as single — BASE
(services ⊕ apps) with every process bound to `central` — the only differences
from single being arch=aarch64 and that this rig is what we cross-compile +
deploy to the Pi (sysroot: third_party/sysroot/rpi4).

TIPC network id: left at the shared default (4711). Machine.tipc_cluster_id is
NOT set here, so run-supervisor.sh keeps clusterid 4711 — the SAME network the
RF/probe host is on. That is deliberate: an rf-theia probe running on the dev
host (or taycann) must reach this Pi's FCs over TIPC, so the two must share the
clusterid. Isolating the Pi (a non-4711 id) would hide its services from the
test driver. `theia manifest rpi4` / `serialize-manifest manifest.rpi4.rig`.
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
        MachineLayer(name="central", arch=Explicit("aarch64"),
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

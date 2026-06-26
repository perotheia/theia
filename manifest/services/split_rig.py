"""services SPLIT rig — the L4-B central + compute 2-machine deploy.

The vehicle's services split across two ECUs (the AUTOSAR Gateway + a domain
ECU), provisioned from ONE manifest so the per-machine slices are consistent and
the singletons land in exactly one place:

  central (the Gateway ECU)  — the deployment-wide SINGLETONS: com (the gRPC/TIPC
    bridge), sm (state mgmt), phm (health), vucm (the vehicle update coordinator),
    crypto, nm, tsync, idsm, fw, osi, diag — PLUS the shared-etcd host (colony) +
    the Mender gateway/proxy (colony mender_role=gateway). Runs its own per + ucm.
  compute (a domain ECU)     — its OWN per (the shared-etcd client) + ucm (the
    per-board AUTOSAR installer) + log + shwa (the Jetson's HW telemetry). NO
    vucm/sm/phm/com singletons — it reaches the central's via TIPC/etcd.

Why split this way (the user's L4-B layout): vucm/sm/phm/com are ONE-per-vehicle
coordinators; running them on every board would mean N campaign coordinators
fighting over one robot. Each board DOES run its own per (marker R/W against the
shared etcd) + ucm (it installs ITS OWN artifacts). log is per-board (local ring).

  serialize-manifest manifest.services.split_rig --attr HW   (rpi4 + jetson, aarch64)
  serialize-manifest manifest.services.split_rig --attr DOCKER (both x86, CI)
"""
from __future__ import annotations

from artheia.manifest.algebra import Append, Explicit
from artheia.manifest.deployment import (
    ApplicationLayer, ApplicationSetLayer, DeploymentLayer, ExecutionLayer,
    MachineLayer, MachineSetLayer, ProcessLayer, _members,
)
from manifest.services.manifest import DEPLOYMENT as BASE
from manifest.services.manifest import PROCESS_NODES  # noqa: F401  (executor.json nodes)

ALL = sorted(p.name for p in _members(BASE.execution.processes))

# The framework manifest binds each FC to EXACTLY ONE machine (no replicas), so
# the singletons-per-deployment principle maps cleanly onto the central/compute
# roles:
#   central = the Gateway/coordinator ECU. The deployment-wide SINGLETONS
#     (com, sm, phm, vucm) + per (the SOLE etcd client — every board's UCM reaches
#     it over TIPC cluster-scope to R/W its ucm_activation_<board> marker) + the
#     central-hosted services (crypto, nm, tsync, idsm, fw, osi, diag, rds).
#   compute = the updatable domain ECU. Its OWN ucm (the per-board AUTOSAR
#     installer) + log (local ring) + shwa (the Jetson HW telemetry). It has NO
#     vucm/sm/phm/com/per — it reaches central's via TIPC + the shared etcd.
# So UCM lives on COMPUTE (the board that takes OTA payloads); central is the
# coordinator (it keeps the log hub + every other singleton). A larger fleet adds
# more compute ECUs the same way (one entry each).
_ON_COMPUTE = sorted({"ucm", "shwa"})
_ON_CENTRAL = [n for n in ALL if n not in set(_ON_COMPUTE)]

# arch-agnostic split: two machines + every process bound to one.
SPLIT = BASE.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={
        MachineLayer(name="central", cores={0, 1, 2, 3},
                     machine_states={"Startup", "Running"}),
        MachineLayer(name="compute", cores={0, 1, 2, 3, 4, 5, 6, 7},
                     machine_states={"Startup", "Running"}),
    }),
    execution=ExecutionLayer(processes={
        *(Append(ProcessLayer(name=n, machine=Explicit("central"))) for n in _ON_CENTRAL),
        *(Append(ProcessLayer(name=n, machine=Explicit("compute"))) for n in sorted(_ON_COMPUTE)),
    }),
    applications=ApplicationSetLayer(applications={
        Append(ApplicationLayer(name="services", host_machine=Explicit("central"))),
    }),
))

# DOCKER — both machines x86_64 (CI / dev box).
DOCKER = SPLIT.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={
        Append(MachineLayer(name="central", arch=Explicit("x86_64"))),
        Append(MachineLayer(name="compute", arch=Explicit("x86_64"))),
    }),
))

# HW — central = Raspberry Pi 4, compute = Jetson AGX Orin (both aarch64).
HW = SPLIT.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={
        Append(MachineLayer(name="central", arch=Explicit("aarch64"))),
        Append(MachineLayer(name="compute", arch=Explicit("aarch64"))),
    }),
))

# Default RIG attr = HW (the L4-B 2-board target).
RIG = HW

# Supervisor tree — reuse the framework services tree; serialize-manifest filters
# each machine's services_sup children to the processes bound to that machine.
from manifest.services.executor import SUPERVISORS  # noqa: F401,E402

"""services SPLIT rig — the L4-B central + compute 2-machine deploy.

The vehicle's services split across two ECUs (the AUTOSAR Gateway + a domain
ECU), provisioned from ONE manifest so the per-machine slices are consistent and
the singletons land in exactly one place:

  central (the Gateway ECU)  — EVERYTHING except the compute-only pair: the
    deployment-wide SINGLETONS (com, sm, phm, vucm), per (the SOLE shared-etcd
    client), log (the trace/log hub), crypto, nm, tsync, idsm, fw, osi, diag, rds
    — PLUS the shared-etcd host (colony) + the Mender gateway/proxy (colony
    mender_role=gateway). 14 FCs.
  compute (a domain ECU)     — ONLY ucm (the per-board AUTOSAR installer) + shwa
    (the Jetson's HW telemetry). 2 FCs. It runs NO per/log/com/sm/phm/vucm — it
    reaches central's per (→ the shared etcd) + the singletons over TIPC
    (cluster scope).

Why split this way (the user's L4-B layout): vucm/sm/phm/com/per/log are
ONE-per-vehicle; running them on every board would mean N coordinators (and N
etcd clients / N log hubs) fighting over one robot. Only ucm is per-board (each
board installs ITS OWN OTA artifacts); shwa is per-board HW telemetry.

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
from manifest.services.manifest import PROCESS_PARAMS  # noqa: F401  (re-export → config/<fc>.json)
from manifest.services.manifest import PROCESS_CONFIG_DEFAULTS  # noqa: F401  (re-export → config-defaults.json)

ALL = sorted(p.name for p in _members(BASE.execution.processes))

# The framework manifest binds each FC to EXACTLY ONE machine (no replicas), so
# the singletons-per-deployment principle maps cleanly onto the central/compute
# roles:
#   central = the Gateway/coordinator ECU. The deployment-wide SINGLETONS
#     (com, sm, phm, vucm) + per (the SOLE etcd client — every board's UCM reaches
#     it over TIPC cluster-scope to R/W its ucm_activation_<board> marker) + log
#     (the trace/log hub) + the central-hosted services (crypto, nm, tsync, idsm,
#     fw, osi, diag, rds). 14 FCs.
#   compute = the updatable domain ECU. ONLY its OWN ucm (the per-board AUTOSAR
#     installer) + shwa (the Jetson HW telemetry). 2 FCs. It has NO
#     vucm/sm/phm/com/per/log — it reaches central's per (→ shared etcd) + the
#     singletons over TIPC cluster scope.
# So UCM lives on COMPUTE (the board that takes OTA payloads); central is the
# coordinator (it keeps the log hub + every other singleton). A larger fleet adds
# more compute ECUs the same way (one entry each).
# KEEP IN SYNC: _ON_COMPUTE below is the source of truth for this split.
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

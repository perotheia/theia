"""services_rig — the framework SERVICES factory rig (arch-agnostic).

The factory-provision rig: the full platform services + supervisor (com, crypto,
log, nm, per, tsync, sm, phm, ucm, vucm, osi, fw, idsm, shwa, diag), bound to one
machine `central`, NO user apps (apps are the day-2 Mender plane, not the factory
runtime). colony provisions a rig from this (runtime+services from S3); the user's
gateway/monitor app is installed later via Mender.

ARCH-AGNOSTIC by design: the machine arch is injected at serialize time —
  artheia serialize-manifest manifest.services.services_rig --attr RIG \
    --arch x86_64  --out dist/services-rig-x86
  artheia serialize-manifest manifest.services.services_rig --attr RIG \
    --arch aarch64 --out dist/services-rig-aarch
ONE rig → per-arch outputs (no duplicate per-arch rig files — the --arch flag
removes that duplication). Default arch is a placeholder overridden by --arch.
"""
from __future__ import annotations

from artheia.manifest.algebra import Append, Explicit
from artheia.manifest.deployment import (
    ApplicationLayer, ApplicationSetLayer, DeploymentLayer, ExecutionLayer,
    MachineLayer, MachineSetLayer, ProcessLayer, _members,
)
from manifest.services.manifest import DEPLOYMENT as BASE
from manifest.services.manifest import PROCESS_NODES  # noqa: F401  (re-export → executor.json nodes)

# Every framework service (no apps). The full platform the factory installs.
ALL = {p.name for p in _members(BASE.execution.processes)}

RIG = BASE.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={
        # arch is a placeholder — `serialize-manifest --arch <token>` overrides it.
        MachineLayer(name="central", arch=Explicit("x86_64"),
                     cores={0, 1, 2, 3}, machine_states={"Startup", "Running"}),
    }),
    execution=ExecutionLayer(processes={
        *(Append(ProcessLayer(name=n, machine=Explicit("central"))) for n in ALL),
    }),
    applications=ApplicationSetLayer(applications={
        Append(ApplicationLayer(name="services", host_machine=Explicit("central"))),
    }),
))

# Supervisor tree — reuse the framework services tree verbatim (all FCs).
from manifest.services.executor import SUPERVISORS  # noqa: F401,E402

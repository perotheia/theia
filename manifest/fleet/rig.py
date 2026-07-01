"""FLEET rpi4 deploy rig — the FULL Theia services slice for an OTA fleet member.

One Raspberry Pi 4 (machine `central`, aarch64) runs EVERY platform service —
the complete stack the OTA campaign targets (vs odd_path's GPS-only KEEP subset):

    com crypto diag fw idsm log nm osi per phm rds sm tsync ucm vucm shwa
    (+ the implicit root supervisor)

This is the full-services release: sm (state mgmt) + phm (health) + ucm (artifact
install engine) + vucm (Mender-facing campaign orchestrator) are all present, so
the FC→PHM→SM escalation chain AND the Mender↔VUCM↔UCM↔EXEC update flow have every
participant on the rig. The supervisor launches each FC from /opt/theia/current
(the release symlink OTA flips); the supervisor binary stays at bin/supervisor.

  THEIA_RIG_MODULE=manifest.fleet.rig theia manifest
  theia dist        # cross-compiles //dist/manifest:central_pkg for aarch64
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
    _members,
)
from manifest.services.manifest import DEPLOYMENT as BASE
# Re-export the per-process node metadata (reporting flag + TIPC addr per node)
# so serialize-manifest folds it into the executor.json worker leaves — without
# it the supervisor registry has no reporting nodes (rtdb trace/loglevel can't
# resolve a node by name). The twin of SUPERVISORS below.
from manifest.services.manifest import PROCESS_NODES  # noqa: F401
from manifest.services.manifest import PROCESS_PARAMS  # noqa: F401  (re-export → config/<fc>.json)
from manifest.services.manifest import PROCESS_CONFIG_DEFAULTS  # noqa: F401  (re-export → config-defaults.json)

# FULL set — keep every service; bind them all to central. (rds is Orin-gated at
# build time via its backend select; it stays in the tree but is a no-op without
# iceoryx — harmless on the Pi.)
ALL = {p.name for p in _members(BASE.execution.processes)}

RIG = BASE.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={
        MachineLayer(name="central", arch=Explicit("aarch64"),
                     cores={0, 1, 2, 3}, machine_states={"Startup", "Running"}),
    }),
    execution=ExecutionLayer(processes={
        # Bind every service to central (no Remove — the full set ships).
        *(Append(ProcessLayer(name=n, machine=Explicit("central"))) for n in ALL),
    }),
    applications=ApplicationSetLayer(applications={
        # Re-bind the base services AA to central (it already bundles every
        # process; just pin the host).
        Append(ApplicationLayer(name="services", host_machine=Explicit("central"))),
    }),
))


# Supervisor tree (serialize-manifest reads module.SUPERVISORS). The framework
# services tree already lists every FC under services_sup — reuse it verbatim;
# nothing is scoped out (full-services rig).
from manifest.services.executor import SUPERVISORS  # noqa: F401,E402

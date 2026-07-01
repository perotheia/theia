"""ODD-PATH LOCAL rig — the same SERVICES slice as manifest.odd_path.rig (the
rpi4 deploy), but bound to an x86_64 `central` for a LOCAL install on taycann
(`theia install odd_path_local` → install/central/ → `theia start`). Aligned to
the rpi4 rig (same KEEP set + supervisor tree); only the arch differs (x86_64 vs
aarch64) so it builds + runs natively here for bring-up / rtdb debugging.

    KEEP:  com, crypto, log, nm, tsync   (+ the implicit root supervisor)
    DROP:  diag, fw, idsm, osi, per, phm, rds, shwa, sm, ucm

  theia install odd_path_local       # build + stage install/central/
  theia start                        # run the staged supervisor
"""
from __future__ import annotations

from artheia.manifest.algebra import Append, Explicit, Remove
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
# Re-export the .art-resolved per-process node metadata (reporting flag +
# TIPC addr per node) so serialize-manifest folds it into executor.json
# worker leaves. Without this the supervisor registry has no reporting
# nodes and rtdb trace/loglevel cannot resolve a node by name.
from manifest.services.manifest import PROCESS_NODES  # noqa: F401
from manifest.services.manifest import PROCESS_PARAMS  # noqa: F401  (re-export → config/<fc>.json)
from manifest.services.manifest import PROCESS_CONFIG_DEFAULTS  # noqa: F401  (re-export → config-defaults.json)

KEEP = {"com", "crypto", "log", "nm", "tsync"}
_ALL = {p.name for p in _members(BASE.execution.processes)}
_DROP = _ALL - KEEP

RIG = BASE.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={
        # Same `central` machine as the rpi4 rig, but x86_64 for a local build.
        MachineLayer(name="central", arch=Explicit("x86_64"),
                     cores={0, 1, 2, 3}, machine_states={"Startup", "Running"}),
    }),
    execution=ExecutionLayer(processes={
        *(Remove(ProcessLayer(name=p)) for p in _DROP),
        *(Append(ProcessLayer(name=n, machine=Explicit("central"))) for n in KEEP),
    }),
    applications=ApplicationSetLayer(applications={
        Remove(ApplicationLayer(name="services")),
        Append(ApplicationLayer(name="services", host_machine=Explicit("central"),
                                processes=frozenset(KEEP))),
    }),
))

from artheia.manifest.supervisor import RestartStrategy, SupervisorNode
from manifest.services.executor import SUPERVISORS as _BASE_SUPS

SUPERVISORS = [
    SupervisorNode(
        name=s.name,
        strategy=s.strategy,
        children=[c for c in s.children if (c in KEEP or c.endswith("_sup"))],
    )
    for s in _BASE_SUPS
]

"""LOCAL test target — single machine, NO network services (pre-merge CI / SIL).

The SINGLE target minus the network-facing pieces: the gRPC bridge process and
its service endpoint. Read it as: SINGLE ⊖ network. The strip is explicit via
Remove(...), so the reader sees exactly what localhost drops.

serialize-manifest manifest.local.rig
"""
from __future__ import annotations

from artheia.manifest.algebra import Append, Explicit, Remove
from artheia.manifest.deployment import (
    ApplicationLayer,
    ApplicationSetLayer,
    DeploymentLayer,
    ExecutionLayer,
    ProcessLayer,
    ServiceInstanceLayer,
    ServiceLayer,
    _members,
)
from manifest.single.rig import (
    PROCESS_NODES as PROCESS_NODES,
    RIG as _SINGLE,
    SUPERVISORS as _SUP,
)

# the services app's process set, minus com (the bundled-process ref must not
# dangle once com is removed from the execution axis).
_services_app = next(a for a in _members(_SINGLE.applications.applications)
                     if a.name == "services")
_services_procs = {p for p in _services_app.processes if p != "com"}

RIG = _SINGLE.combine(DeploymentLayer(
    execution=ExecutionLayer(processes={
        Remove(ProcessLayer(name="com")),
    }),
    service=ServiceLayer(instances={
        Remove(ServiceInstanceLayer(name="com_bridge")),
    }),
    applications=ApplicationSetLayer(applications={
        # an app's `processes` set UNIONS on combine, so we can't shrink it by
        # Append — replace the whole services app: Remove then Append a fresh one
        # without com.
        Remove(ApplicationLayer(name="services")),
        Append(ApplicationLayer(name="services", host_machine=Explicit("central"),
                                processes=_services_procs)),
    }),
))

# com is removed from the deployment; drop it from the supervisor tree too so the
# sup tree has no dangling child.
SUPERVISORS = [
    s if "com" not in getattr(s, "children", [])
    else type(s)(name=s.name, strategy=s.strategy,
                 children=[c for c in s.children if c != "com"])
    for s in _SUP
]

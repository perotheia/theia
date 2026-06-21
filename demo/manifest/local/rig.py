"""LOCAL test target — single machine, NO network services, NO privileged FCs.

The SINGLE target minus the pieces that can't run UNPRIVILEGED on a plain dev
host (pre-merge CI / SIL): the gRPC bridge process AND the capability-/root-
needing FCs. Read it as: SINGLE ⊖ network ⊖ privileged. The strip is explicit via
Remove(...), so the reader sees exactly what localhost drops.

Dropped FCs:
  com   — the gRPC bridge process + its service endpoint (network).
  fw    — Firewall: nft -f needs CAP_NET_ADMIN ("Operation not permitted").
  idsm  — IDS-Manager: eBPF + NETLINK_SOCK_DIAG need CAP_BPF/root.
  nm    — Network-Management: rtnetlink reads/touches host links/addrs.
  osi   — OS-Interface: cgroup v2 + nvpmodel power need root.
  rds   — Raw-Data-Stream: iceoryx RoudiBroker needs root + shared memory.
The full set runs in the container/HW targets (single / split), where caps +
an isolated netns make them safe.

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

# FCs the LOCAL (capless dev host) target drops: the gRPC bridge (network) + the
# capability-/root-needing FCs. Each is removed from the execution axis, the
# services app's process set, and the supervisor tree.
_DROPPED = {"com", "fw", "idsm", "nm", "osi", "rds"}

# the services app's process set, minus the dropped FCs (a bundled-process ref
# must not dangle once its process leaves the execution axis).
_services_app = next(a for a in _members(_SINGLE.applications.applications)
                     if a.name == "services")
_services_procs = {p for p in _services_app.processes if p not in _DROPPED}

RIG = _SINGLE.combine(DeploymentLayer(
    execution=ExecutionLayer(processes={
        Remove(ProcessLayer(name=fc)) for fc in _DROPPED
    }),
    service=ServiceLayer(instances={
        # com is the only dropped FC with a bridge service instance; the others
        # are control-plane FCs with no provided ServiceInstance to strip.
        Remove(ServiceInstanceLayer(name="com_bridge")),
    }),
    applications=ApplicationSetLayer(applications={
        # an app's `processes` set UNIONS on combine, so we can't shrink it by
        # Append — replace the whole services app: Remove then Append a fresh one
        # without the dropped FCs.
        Remove(ApplicationLayer(name="services")),
        Append(ApplicationLayer(name="services", host_machine=Explicit("central"),
                                processes=_services_procs)),
    }),
))

# The dropped FCs are gone from the deployment; drop them from the supervisor
# tree too so no sup node has a dangling child.
SUPERVISORS = [
    s if not (_DROPPED & set(getattr(s, "children", [])))
    else type(s)(name=s.name, strategy=s.strategy,
                 children=[c for c in s.children if c not in _DROPPED])
    for s in _SUP
]

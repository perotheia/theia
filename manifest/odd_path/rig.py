"""ODD-PATH rpi4 deploy rig — the Theia SERVICES slice that feeds odd-path-monitor.

One Raspberry Pi 4 (machine `central`, aarch64) runs the platform services the
odd-path-monitor GUI depends on for its GPS/odometry feed over PG + remote
observability:

    KEEP:  com, crypto, log, nm, per, tsync   (+ the implicit root supervisor)
    DROP:  diag, fw, idsm, osi, phm, rds, shwa, sm, ucm

per is the config gatekeeper (etcd-backed): NmConfig (wifi profiles + priority,
auto_connect, auto_vpn) is stored in etcd and pushed to nm via ConfigUpdated, so
an operator enrolls wifi from tdb/rtdb (set priority per entry) without a static
file. Needs etcd on the box (provision installs it; etcd_machine=central).

tsync is the NavSatFix/Odometry producer (build --define gps=rtk on the real Pi,
or gps=fake for bench); the supervisor is the PG allocator the GUI's pg_join
needs; com is the gRPC bridge (rtdb over VPN); nm observes the tailscale tunnel;
crypto/log round out the production set. The Qt app itself is compiled ON the Pi
(Qt deps), NOT in this slice.

  THEIA_RIG_MODULE=manifest.odd_path.rig theia manifest
  theia dist        # cross-compiles //dist/manifest:central_pkg for aarch64
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

# The services the GPS path + production rig need; everything else is dropped.
KEEP = {"com", "crypto", "log", "nm", "per", "tsync"}
_ALL = {p.name for p in _members(BASE.execution.processes)}
_DROP = _ALL - KEEP

RIG = BASE.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={
        MachineLayer(name="central", arch=Explicit("aarch64"),
                     cores={0, 1, 2, 3}, machine_states={"Startup", "Running"}),
    }),
    execution=ExecutionLayer(processes={
        # Drop the services this rig doesn't ship (monoid Remove, by `name`) ...
        *(Remove(ProcessLayer(name=p)) for p in _DROP),
        # ... and bind the kept services to central.
        *(Append(ProcessLayer(name=n, machine=Explicit("central"))) for n in KEEP),
    }),
    applications=ApplicationSetLayer(applications={
        # Replace the base services AA: drop it (it bundles the removed
        # processes) and re-add one scoped to only KEEP, bound to central.
        Remove(ApplicationLayer(name="services")),
        Append(ApplicationLayer(name="services", host_machine=Explicit("central"),
                                processes=frozenset(KEEP))),
    }),
))


# Supervisor tree (serialize-manifest reads module.SUPERVISORS). Reuse the
# framework services tree but SCOPE services_sup to only the KEEP processes —
# else the supervisor tries to fork dropped (un-shipped) children at boot.
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

"""RPI4 SERVICE-TEST target — one Raspberry Pi 4 (machine `central`) runs the
platform SERVICES only (no demo apps).

A copy of the SINGLE rig retargeted to the lab rpi4 (rig1-central, 10.0.0.22):
Debian 13 trixie, aarch64, 4 cores. The rpi4 is for SERVICE testing (tsync GNSS
RTK, nm, fw, …), not the Demo3Way counter apps — so this rig binds the services
to `central` and REMOVES the demo app processes that BASE (= services ⊕ apps)
pulls in. The apps aren't gen-app'd on a service-test workspace anyway, so
leaving them in would make `theia dist` fail on //apps/Demo3WayP*.

TIPC network id: left at the shared default (4711) — same network the RF/probe
host is on, so a cross-machine probe/tdb reaches this Pi (see THEIA_TIPC_SCOPE).
`theia manifest rpi4` / `serialize-manifest manifest.rpi4.rig`.

--- Monoid delete syntax (artheia.manifest.algebra) -------------------------
A set field (execution.processes, applications.applications) holds EITHER plain
members OR a set of EDITS — never mixed. To DROP a member contributed by a lower
layer (here BASE), the overlay carries `Remove(<member>)` edits:

  * Remove(X)  drops the set member matching X. For an Identifiable (every
    *Layer is), the match is by the IDENTITY field only (`name`), so you write
    `Remove(ProcessLayer(name="p1"))` — the other fields are irrelevant.
  * Append(X)  adds X, or merge-by-identity if a same-name member exists.
  * In one edit set, all Remove()s apply before Append()s (deterministic
    "replace": drop the old, then add the new).
  * combine() folds the overlay's edits over the lower layer's resolved set.

So `execution=ExecutionLayer(processes={Remove(ProcessLayer(name=p)) ...})`
deletes those processes from BASE's set; the surviving services are then bound
to `central` by a second Append pass below.
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
    ServiceInstanceLayer,
    ServiceLayer,
    _members,
)
from manifest.apps.manifest import DEPLOYMENT as APPS
from manifest.assemble import (
    BASE,
    BASE_PROCESS_NODES,
    BASE_SUPERVISORS,
    PROCESS_NAMES,
)

# Derive the demo-app pieces to drop straight from the apps manifest (so this
# stays correct if the demo changes): the app PROCESS names, the app SERVICE
# instances (whose provided_by is an app process — leaving these orphaned trips
# the deployment validator), and the apps APPLICATION group.
_APP_PROCS = {p.name for p in _members(APPS.execution.processes)}
_APP_SVCS = [s.name for s in _members(APPS.service.instances)]
_APP_APPS = [a.name for a in _members(APPS.applications.applications)]
_SVC_PROCS = [n for n in PROCESS_NAMES if n not in _APP_PROCS]

RIG = BASE.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={
        MachineLayer(name="central", arch=Explicit("aarch64"),
                     cores={0, 1, 2, 3}, machine_states={"Startup", "Running"}),
    }),
    execution=ExecutionLayer(processes={
        # DELETE the demo app processes BASE contributed (monoid Remove, matched
        # by the identity field `name`) ...
        *(Remove(ProcessLayer(name=p)) for p in _APP_PROCS),
        # ... and bind the surviving SERVICE processes to central.
        *(Append(ProcessLayer(name=n, machine=Explicit("central"))) for n in _SVC_PROCS),
    }),
    # Drop the app service instances too — else they reference the just-removed
    # app processes and the validator refuses (provided_by not in execution).
    service=ServiceLayer(instances={
        Remove(ServiceInstanceLayer(name=s)) for s in _APP_SVCS
    }),
    applications=ApplicationSetLayer(applications={
        # Keep the services AA on central; DROP the apps AA entirely.
        Append(ApplicationLayer(name="services", host_machine=Explicit("central"))),
        *(Remove(ApplicationLayer(name=a)) for a in _APP_APPS),
    }),
))

SUPERVISORS = BASE_SUPERVISORS
# Drop the app processes from the per-process supervisor metadata too, so the
# executor tree the supervisor builds has no app children.
PROCESS_NODES = {k: v for k, v in BASE_PROCESS_NODES.items() if k not in _APP_PROCS}

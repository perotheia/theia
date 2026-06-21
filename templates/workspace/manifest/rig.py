"""manifest/rig.py — your deploy rig (one machine to start).

The rig is the Python module artheia reads to know which processes deploy where.
`@rig_myapp` in MODULE.bazel points here; `theia install` / `theia manifest`
materialize it.

It is a :class:`DeploymentLayer` on the orthogonal-ARA engine
(:mod:`artheia.manifest.deployment`): it combines your workspace's generated
apps manifest (the BASE, machines left open) with a deploy delta binding every
process to one machine ("central"). The apps manifest is gen-manifest output
(`artheia gen-manifest system/myapp/component.art manifest/apps/manifest.py`);
until you run it the import is guarded → an empty deployment, which still
imports + serializes so you can verify the toolchain.

See the Theia docs (`docs/skills/theia/references/deployment.md`) and the
in-repo `demo/manifest/single/rig.py` for a worked example.
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
    _members as _set_members,
)

# The generated apps manifest (a base DeploymentLayer, machines open). Guarded:
# absent until the first `gen-manifest` → a fresh workspace is an empty deploy.
try:
    from manifest.apps.manifest import DEPLOYMENT as _APPS
except Exception:
    _APPS = DeploymentLayer()

_PROCESS_NAMES = sorted(p.name for p in _set_members(_APPS.execution.processes))

# Deploy delta: one machine "central"; every process bound to it; one AA.
RIG = _APPS.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={
        MachineLayer(name="central", arch=Explicit("x86_64"),
                     cores={0, 1, 2, 3}, machine_states={"Startup", "Running"}),
    }),
    execution=ExecutionLayer(processes={
        Append(ProcessLayer(name=n, machine=Explicit("central")))
        for n in _PROCESS_NAMES
    }),
    applications=ApplicationSetLayer(applications={
        Append(ApplicationLayer(name="apps", host_machine=Explicit("central"))),
    }),
))

try:
    from manifest.apps.executor import SUPERVISORS
except Exception:
    SUPERVISORS = []

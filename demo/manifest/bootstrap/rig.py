"""demo deploy rig — one machine ("central") running this workspace's apps.

A :class:`DeploymentLayer` on the orthogonal-ARA engine
(:mod:`artheia.manifest.deployment`). It combines the workspace's generated
apps manifest (the BASE — open machines) with a deploy delta: one machine and
every process bound to it. `theia manifest` / `theia install` / `theia start`
read the RIG export.

The apps manifest is gen-manifest output. Until you run it the import fails, so
it is guarded — a fresh workspace resolves to an EMPTY deployment (one machine,
no processes), which is enough to verify the toolchain (theia manifest / install
/ start). As you add compositions to system/apps/component.art and regenerate
(`artheia gen-manifest system/apps/component.art manifest/apps/manifest.py`),
the processes + applications flow in automatically.

See $THEIA_ROOT/docs/skills/theia/references/deployment.md.
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
)

# The generated apps manifest (a base DeploymentLayer with machines left open).
# Not present until the first `gen-manifest` — guard the import so a fresh
# workspace still imports + serializes (an empty deployment).
try:
    from manifest.apps.manifest import DEPLOYMENT as _APPS
except Exception:               # not generated yet → empty workspace
    _APPS = DeploymentLayer()

# Every process the apps base declares (bind each to the one machine below).
from artheia.manifest.deployment import _members as _set_members
_PROCESS_NAMES = sorted(p.name for p in _set_members(_APPS.execution.processes))

# The deploy delta: one machine "central"; every app process bound to it; one
# AA ("apps") grouping them on that host. Combined onto the apps base.
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

# Optional supervisor sidecar (gen-manifest writes manifest/apps/executor.py).
# serialize-manifest reads SUPERVISORS off this module if present.
try:
    from manifest.apps.executor import SUPERVISORS
except Exception:
    SUPERVISORS = []

# Per-process node/module metadata for the executor.json worker leaves.
# gen-manifest emits it onto manifest.apps.manifest; empty until first run.
try:
    from manifest.apps.manifest import PROCESS_NODES
except Exception:
    PROCESS_NODES = {}

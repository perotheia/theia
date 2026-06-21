"""Shared assembly of the demo workspace's manifests.

Every test-target rig starts from the SAME assembled base: the framework's
services manifest combined with the demo's apps manifest, plus their two
hand-editable supervisor subtrees merged under one root. A target rig then
applies only its deploy transform (machines + bindings), so each rig reads as
just its delta:

    from manifest.assemble import BASE, BASE_SUPERVISORS, PROCESS_NAMES
    RIG = BASE.combine(DeploymentLayer(... this target's machines + bindings ...))
    SUPERVISORS = BASE_SUPERVISORS

`manifest.services` resolves from the framework (intrinsic); `manifest.apps`
from the demo workspace (extrinsic) — one `manifest` namespace package across
both trees.
"""
from __future__ import annotations

from artheia.manifest.deployment import _members
from artheia.manifest.supervisor import RestartStrategy, SupervisorNode
from manifest.apps.executor import SUPERVISORS as _APPS_SUP
from manifest.apps.manifest import DEPLOYMENT as APPS
from manifest.services.executor import SUPERVISORS as _SVC_SUP
from manifest.services.manifest import DEPLOYMENT as SERVICES

# The assembled deployment (machines still open — a target rig binds them).
BASE = SERVICES.combine(APPS)

# Every process name in the assembled base (services FCs + demo apps), for a
# target rig to bind to machines.
PROCESS_NAMES = sorted(p.name for p in _members(BASE.execution.processes))

# The merged supervisor tree: one root over each source's <fg>_sup subtree
# (the per-source roots are dropped). Hand-editable per source via their
# write-once executor.py; assembled here.
_SUBTREES = [n for n in (_SVC_SUP + _APPS_SUP) if n.name != "root"]
BASE_SUPERVISORS = [
    SupervisorNode(name="root", strategy=RestartStrategy.ONE_FOR_ALL,
                   children=[n.name for n in _SUBTREES]),
    *_SUBTREES,
]

"""Apps supervisor-tree sidecar — GENERATED from system/apps/component.art by gen-manifest.

One ``app_sup`` node (one_for_one) whose children are this manifest's app
members. The generic rig grafts it onto the services supervisor tree's empty
``app_sup`` mount. Regenerate with applications.py (gen-manifest); the app set is
.art-derived, so DON'T hand-edit the children — change the cluster in the .art.
"""
from __future__ import annotations

from artheia.manifest.supervisor import RestartStrategy, SupervisorNode

# Every app member from the generated cluster sections (one_for_one: apps are
# peer-independent, like the platform app_sup).
SUPERVISORS: list[SupervisorNode] = [
    SupervisorNode(
        name="app_sup",
        strategy=RestartStrategy.ONE_FOR_ONE,
        children=['p1', 'p2', 'p3', 'p4'],
    ),
]

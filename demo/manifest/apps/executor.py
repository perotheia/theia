"""Supervisor tree for demo/system/apps/component.art — hand-editable.

Regenerate only with --force. gen-manifest derives an initial tree from the
DEPLOYMENT: a ``root`` supervisor (one_for_all) whose children are one
``<function_group>_sup`` per function group, each (one_for_one) parenting its
processes BY NAME. ``SupervisorNode.children`` is a list of names; leaves
resolve to the matching process at build time. Once written this file is YOURS
to edit (restart strategies, grouping) — a plain ``gen-manifest`` run keeps it
untouched.
"""
from __future__ import annotations

from artheia.manifest.supervisor import RestartStrategy, SupervisorNode

SUPERVISORS: list[SupervisorNode] = [
    SupervisorNode(
        name="root",
        strategy=RestartStrategy.ONE_FOR_ALL,
        children=["applications_sup"],
    ),
    SupervisorNode(
        name="applications_sup",
        strategy=RestartStrategy.ONE_FOR_ONE,
        children=['p1', 'p2', 'p3', 'p4'],
    ),
]

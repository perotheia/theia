"""Hand-authored supervisor tree — sidecar for :mod:`service.py`.

The :data:`SUPERVISORS` list is the OTP-style hierarchical supervisor
tree (see ~/org/1.org). It is **manually written** — there is no .art
declaration for the supervisor hierarchy (``system.art`` only carries
the FC compositions + the Supervisor/GatewayBridge process forward-decls,
not the restart strategy or the child grouping).

It lives in this separate module so that regenerating ``service.py``
from ``platform/system/system.art`` (the FC list) does NOT clobber the
tree. ``service.py`` re-exports it as ``service.SUPERVISORS`` so existing
consumers (``artheia.manifest.platform``, ``FcLayer``, ``FcSoftware``)
keep working unchanged — edit the tree HERE.

Children name either another :class:`SupervisorNode` or a
:class:`Process` from the rig's execution manifests. ``app_sup`` ships
empty in the platform base — application leaves are added by the rig
layer (e.g. the apps rig Overrides app_sup with apps_p1/p2/p3), each
resolving to a Process with its own ``start_cmd``, exactly like an FC
leaf.

Restart-strategy choices match the OTP design:

- ``root`` is the catastrophic-escalation point. ``one_for_all`` so a
  top-level escalation reboots the entire subtree.
- ``ar_sup`` is ``rest_for_one`` — if ``core_sup`` fails, ``app_sup``
  restarts too (apps can't survive without the platform).
- ``core_sup`` is ``rest_for_one`` — within the platform layer, ``exec``
  precedes the others. If it crashes, downstream peers restart in
  declared order.
- Per-domain sub-supervisors are ``one_for_one`` (peer-independent).
- ``app_sup`` is ``one_for_one`` — vendor apps are independent.
"""

from __future__ import annotations

from artheia.manifest.supervisor import (
    RestartStrategy,
    SupervisorNode,
)

SUPERVISORS: list[SupervisorNode] = [
    SupervisorNode(
        name="root",
        strategy=RestartStrategy.ONE_FOR_ALL,
        children=["ar_sup"],
        tombstone_dir="/tmp/tombstones",
    ),
    SupervisorNode(
        name="ar_sup",
        strategy=RestartStrategy.REST_FOR_ONE,
        children=["core_sup", "app_sup"],
    ),
    SupervisorNode(
        name="core_sup",
        strategy=RestartStrategy.REST_FOR_ONE,
        children=[
            # Leaves first, in declared order — `exec` is the execution
            # manager, so it must come up first. core / crypto / sm
            # follow because the rest_for_one cascade order is meaningful.
            "exec", "core", "crypto", "sm",
            # Per-domain sub-supervisors after the leaves.
            "network_sup", "host_svc_sup", "pltf_sup", "drv_sup",
        ],
    ),
    SupervisorNode(
        name="drv_sup",
        strategy=RestartStrategy.ONE_FOR_ONE,
        # Driver / data-path mount-point. EMPTY in standalone theia.git — the
        # gateway (GatewayBridge: GatewayService + CmpGwService + the PSP
        # Kcan_Bus / Flexray_Bus mega-nodes) lives in a consuming workspace
        # (gataway_ws), which Appends "gateway" here. Kept as its own driver
        # sub-supervisor so a downstream bus driver lands under it, not among
        # the host services. See docs/tasks/TODO/repo-split-standalone-theia.md.
        children=[],
    ),
    SupervisorNode(
        name="network_sup",
        strategy=RestartStrategy.ONE_FOR_ONE,
        children=["nm", "osi", "idsm", "diag", "tsync"],
    ),
    SupervisorNode(
        name="host_svc_sup",
        strategy=RestartStrategy.ONE_FOR_ONE,
        # "log" = the log[trace] FC (services/log/): the ring-buffer trace
        # hub — TraceStreamPump (raw record fan-out, tipc 0x80010013) +
        # TraceCtl (Subscribe/Configure control plane, tipc 0x80010014).
        # It MUST be forked: tdb tracecat / artheia.observer Subscribe to
        # TraceCtl, and per-node Tracer records egress to the pump. Without
        # it, `tdb trace <node>` stores config but nothing receives the
        # records and Subscribe has no listener. (The earlier "no daemon —
        # spec-completeness only" note predated the trace hub; the hub now
        # exists and is exactly this child.)
        #
        # "com" = the services/com FC (ComDaemon + the ComGrpcProxy gRPC
        # bridge). It's a real packaged FC in `cluster Services` and the GUI /
        # rtdb / rf trace surface — it MUST be forked by the supervisor so the
        # stack comes up complete without launching com by hand.
        #
        # "roudi" = the iceoryx broker (services/rds) — a NATIVE prebuilt binary
        # (iox-roudi, not a Theia FC) that owns the shared-memory pools the
        # requires_rds FCs publish/subscribe through. FIRST in the list so it's
        # up before any RDS producer/consumer registers with it. (This is the
        # real "rds" — the old daemon-less rds phantom is finally a thing.)
        children=["roudi", "log", "per", "com"],
    ),
    SupervisorNode(
        name="pltf_sup",
        strategy=RestartStrategy.ONE_FOR_ONE,
        # "camera" is listed in ~/org/1.org as an example app — it lands
        # here if a rig layer adds an FC with that short name; otherwise
        # quietly skipped. ``shwa`` and ``fw`` aren't in any branch in
        # ~/org/1.org so they're grouped here as platform-level extras.
        children=["phm", "camera", "ucm", "vucm", "shwa", "fw"],
    ),
    SupervisorNode(
        name="app_sup",
        strategy=RestartStrategy.ONE_FOR_ONE,
        # Application leaves. The platform base ships NONE — apps belong
        # to the rig. A rig layer Overrides app_sup's children with its
        # own application Process names (e.g. the apps rig adds
        # apps_p1/p2/p3), each resolving to a Process in the rig's
        # execution manifests. (Earlier this used an AUTO_APPS_CHILDREN
        # sentinel that auto-expanded SwComponents with a synthetic
        # vendor/apps/<name>/daemon.sh start_cmd — dropped; apps now
        # carry a real start_cmd in their execution manifest, same as FCs.)
        children=[],
    ),
]


__all__ = ["SUPERVISORS"]

"""services rig — the framework RUNTIME rig, ROLE-based (arch-agnostic).

The deploy delta for the generated services base (manifest.services.manifest).
It declares the deployment ROLES — there is no longer a single/split rig pair;
ONE rig names the roles and the GS Distribution picks which materialize onto
which boards ($name).

Two roles:
  central (role="central") — the MASTER: the full platform services + supervisor
    (com, crypto, log, nm, per, tsync, sm, phm, ucm, vucm, osi, fw, idsm, shwa,
    diag) + etcd + the deployment-wide singletons (per/sm/phm/com/vucm) + the
    mender gateway/proxy. colony provisions etcd + wifi utils + mender-gw here.
  zonal  (role="zonal")   — a ZONE-OF-RESPONSIBILITY worker: the minimal set
    (ucm + shwa) + a mender agent reaching the server via central's proxy. It
    runs NO per/log/com/sm/phm/vucm — it reaches central's singletons over TIPC
    (cluster scope) and central's shared etcd. colony provisions ucm + agent.

No user apps (apps are the day-2 Mender plane, not the factory runtime). colony
provisions a board from the role slice (runtime+services from S3); the user's
gateway/monitor app is installed later via Mender.

Two exports:
  RIG   — central only (the arity-1 single-master runtime; the common case).
  MULTI — central + zonal (arity-2; a multi-board Distribution serializes this
          and binds each role to a board).
`theia manifest services` serializes RIG; `--attr MULTI` serializes the
2-role variant. Addressed as `services` (manifest/services/rig.py →
manifest.<target>.rig), symmetric with the user-app side (`theia manifest
single`).

ARCH-AGNOSTIC by design: each machine's arch is injected at serialize time —
  artheia serialize-manifest manifest.services.rig --attr RIG   --arch x86_64
  artheia serialize-manifest manifest.services.rig --attr MULTI --arch aarch64
ONE rig → per-arch outputs (the --arch flag removes per-arch rig duplication).
Default arch is a placeholder overridden by --arch.
"""
from __future__ import annotations

from artheia.manifest.algebra import Append, Explicit
from artheia.manifest.deployment import (
    ApplicationLayer, ApplicationSetLayer, DeploymentLayer, ExecutionLayer,
    MachineLayer, MachineSetLayer, ProcessLayer, _members,
)
from manifest.services.manifest import DEPLOYMENT as BASE
from manifest.services.manifest import PROCESS_NODES  # noqa: F401  (re-export → executor.json nodes)
from manifest.services.manifest import PROCESS_PARAMS  # noqa: F401  (re-export → config/<fc>.json)
from manifest.services.manifest import PROCESS_CONFIG_DEFAULTS  # noqa: F401  (re-export → config-defaults.json)

# Every framework service (no apps). The full platform the master installs.
ALL = {p.name for p in _members(BASE.execution.processes)}

# The zone-of-responsibility slice: only the per-board FCs. ucm = the per-board
# AUTOSAR installer (each board installs ITS OWN OTA artifacts); shwa = per-board
# HW telemetry. Everything else is a central singleton reached over TIPC.
ZONAL = {"ucm", "shwa"} & ALL


def _central_machine():
    # role="central" + etcd → the master. arch is a placeholder overridden by
    # `serialize-manifest --arch`.
    return MachineLayer(name="central", role=Explicit("central"),
                        arch=Explicit("x86_64"), etcd=Explicit(True),
                        cores={0, 1, 2, 3},
                        machine_states={"Startup", "Running"})


def _zonal_machine():
    # role="zonal" — a worker board. No etcd (reaches central's shared etcd).
    return MachineLayer(name="zonal", role=Explicit("zonal"),
                        arch=Explicit("x86_64"),
                        cores={0, 1}, machine_states={"Startup", "Running"})


# ── RIG — central only (arity 1: the single-master runtime, the common case). ──
RIG = BASE.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={_central_machine()}),
    execution=ExecutionLayer(processes={
        *(Append(ProcessLayer(name=n, machine=Explicit("central"))) for n in ALL),
    }),
    applications=ApplicationSetLayer(applications={
        Append(ApplicationLayer(name="services", host_machine=Explicit("central"))),
    }),
))

# ── MULTI — central + zonal (arity 2: a multi-board Distribution). ────────────
# Central runs every singleton + control-plane FC; the zone board runs only its
# per-board installer (ucm). shwa is PER-BOARD HW telemetry, so it runs on BOTH
# via a set-valued `machines` (the serializer fans it onto each board — see
# _proc_machines). Everything not shwa/ucm is central-only (the singletons
# com/per/sm/phm land there by construction).
_PER_BOARD = {"shwa"} & ALL          # runs on every board (HW telemetry)
_ZONE_ONLY = (ZONAL - _PER_BOARD)    # the zone board's exclusive FCs (ucm)
_CENTRAL_ONLY = ALL - ZONAL          # everything but ucm/shwa → central only
MULTI = BASE.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={_central_machine(), _zonal_machine()}),
    execution=ExecutionLayer(processes={
        *(Append(ProcessLayer(name=n, machine=Explicit("central")))
          for n in _CENTRAL_ONLY),
        *(Append(ProcessLayer(name=n, machine=Explicit("zonal")))
          for n in _ZONE_ONLY),
        # shwa on BOTH boards — set-valued machines fans it out per board.
        *(Append(ProcessLayer(name=n, machines={"central", "zonal"}))
          for n in _PER_BOARD),
    }),
    applications=ApplicationSetLayer(applications={
        Append(ApplicationLayer(name="services", host_machine=Explicit("central"))),
    }),
))

# Supervisor tree — the per-role trees (central = all FCs; zonal = ucm + shwa)
# live in executor.py. serialize-manifest slices SUPERVISORS per machine by the
# processes placed there, so one tree covers both roles.
from manifest.services.executor import SUPERVISORS  # noqa: F401,E402

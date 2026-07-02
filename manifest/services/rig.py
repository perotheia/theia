"""services rig — the framework RUNTIME rig, ROLE-based (arch-agnostic).

The deploy delta for the generated services base (manifest.services.manifest).
It declares the deployment ROLES — there is no longer a single/split rig pair;
ONE rig names the roles and the GS Distribution picks which materialize onto
which boards ($name).

Two roles (the machine NAME is the role — colony pulls manifest/<role>/):
  master (role="master") — the coordinator: the full platform services +
    supervisor (com, crypto, log, nm, per, tsync, sm, phm, ucm, vucm, osi, fw,
    idsm, shwa, diag) + etcd + the deployment-wide singletons (per/sm/phm/com/
    vucm) + the mender gateway/proxy. colony provisions etcd + wifi + mender-gw.
  zonal  (role="zonal")  — a ZONE-OF-RESPONSIBILITY worker: the minimal set
    (ucm + shwa) + a mender agent reaching the server via the master's proxy. It
    runs NO per/log/com/sm/phm/vucm — it reaches the master's singletons over TIPC
    (cluster scope) and the shared etcd. ONE zonal slice serves N workers; the
    day-2 app deploy names each (compute/frontal/…).

No user apps (apps are the day-2 Mender plane, not the factory runtime). colony
provisions a board from the ROLE slice (runtime+services from S3); the user's
gateway/monitor app is installed later via Mender.

Two exports:
  RIG   — master only (the arity-1 single-master runtime; the common case).
  MULTI — master + zonal (arity ≥2; a multi-board Distribution serializes this,
          one master + N zonal workers, each role bound to a board day-2).
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


# The runtime manifest is keyed by ROLE, not board name: colony pulls the
# manifest/<role>/ slice at orchestrate time (master or zonal), and a worker board
# is role-generic until the day-2 app deploy names it (compute/frontal/…). So the
# MACHINE NAME here IS the role — master (the coordinator) and zonal (a worker).
def _master_machine():
    # role="master" + etcd → the coordinator. arch is a placeholder overridden by
    # `serialize-manifest --arch`.
    return MachineLayer(name="master", role=Explicit("master"),
                        arch=Explicit("x86_64"), etcd=Explicit(True),
                        cores={0, 1, 2, 3},
                        machine_states={"Startup", "Running"})


def _zonal_machine():
    # role="zonal" — a worker board (zone of responsibility). No etcd (reaches the
    # master's shared etcd). ONE zonal slice serves every worker; the day-2 app
    # deploy binds a specific board name to it.
    return MachineLayer(name="zonal", role=Explicit("zonal"),
                        arch=Explicit("x86_64"),
                        cores={0, 1}, machine_states={"Startup", "Running"})


# ── RIG — master only (arity 1: the single-master runtime, the common case). ──
RIG = BASE.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={_master_machine()}),
    execution=ExecutionLayer(processes={
        *(Append(ProcessLayer(name=n, machine=Explicit("master"))) for n in ALL),
    }),
    applications=ApplicationSetLayer(applications={
        Append(ApplicationLayer(name="services", host_machine=Explicit("master"))),
    }),
))

# ── MULTI — master + zonal (arity ≥2: a multi-board Distribution). ────────────
# The master runs every singleton + control-plane FC; a zonal worker runs only its
# per-board installer (ucm). shwa is PER-BOARD HW telemetry, so it runs on BOTH
# roles via a set-valued `machines` (the serializer fans it onto each — see
# _proc_machines). Everything not shwa/ucm is master-only (the singletons
# com/per/sm/phm land there by construction). ONE zonal slice serves N workers.
_PER_BOARD = {"shwa"} & ALL          # runs on every board (HW telemetry)
_ZONE_ONLY = (ZONAL - _PER_BOARD)    # the zone board's exclusive FCs (ucm)
_MASTER_ONLY = ALL - ZONAL           # everything but ucm/shwa → master only
MULTI = BASE.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={_master_machine(), _zonal_machine()}),
    execution=ExecutionLayer(processes={
        *(Append(ProcessLayer(name=n, machine=Explicit("master")))
          for n in _MASTER_ONLY),
        *(Append(ProcessLayer(name=n, machine=Explicit("zonal")))
          for n in _ZONE_ONLY),
        # shwa on BOTH roles — set-valued machines fans it out per board.
        *(Append(ProcessLayer(name=n, machines={"master", "zonal"}))
          for n in _PER_BOARD),
    }),
    applications=ApplicationSetLayer(applications={
        Append(ApplicationLayer(name="services", host_machine=Explicit("master"))),
    }),
))

# Supervisor tree — the per-role trees (master = all FCs; zonal = ucm + shwa)
# live in executor.py. serialize-manifest slices SUPERVISORS per machine by the
# processes placed there, so one tree covers both roles.
from manifest.services.executor import SUPERVISORS  # noqa: F401,E402

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

ONE export — RIG — the master + zonal deployment. Both roles ALWAYS exist in the
manifest; arity is a DEPLOY-TIME fact (colony deploys role=master → the master
slice, role=zonal → the zonal slice, fanning the single zonal slice onto N
boards). There is no master-only variant: a 1-board deploy simply never
materializes the zonal slice. Addressed as `services` (manifest/services/rig.py
→ manifest.services.rig), symmetric with the user-app side (`theia manifest
single`).

ARCH-AGNOSTIC by design: each machine's arch is injected at serialize time —
  artheia serialize-manifest manifest.services.rig --arch x86_64
  artheia serialize-manifest manifest.services.rig --arch aarch64
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
from manifest.services.manifest import PROCESS_NODES as _BASE_PROCESS_NODES
from manifest.services.manifest import PROCESS_PARAMS  # noqa: F401  (re-export → config/<fc>.json)
from manifest.services.manifest import PROCESS_CONFIG_DEFAULTS  # noqa: F401  (re-export → config-defaults.json)

# ── Provisioning-time boot gate (run_on_start=false) ────────────────────────
# This RIG's executor.json is the RUNTIME (base) plane a rig runs AFTER
# `theia provision` but BEFORE its SWP lands. A few services can't do useful
# work until the SWP supplies the rig's real config — its known eth/wifi
# interface names, the site nftables policy, the PTP interface:
#   nm     — needs the real interface names + CAP_NET_ADMIN; touches live NICs.
#   fw     — applies nftables to real interfaces; the SWP ships the site policy
#            (fw stays enabled=true by default — provisioning has no SWP config
#            to lock down yet, so there is nothing for it to enforce, and it
#            would only default-drop against interfaces it doesn't know).
#   tsync  — the PTP/PHC interface comes from the SWP; bare, it just degrades
#            to the wall clock, so booting it pre-SWP buys nothing.
# per is deliberately NOT gated: UCM needs it to persist SWP update state, and a
# provisioned rig already has host etcd, so per works from first boot.
#
# These are DEFINED in the tree but not booted (spec.cpp: run_on_start=false =
# define-but-don't-boot). The SWP's OWN executor.json carries the full tree with
# every node run_on_start=true, and REPLACES this one on deploy — so the SWP
# re-enables them with no undo step here. serialize-manifest reads run_on_start
# from PROCESS_NODES meta (cli.py), so augmenting the re-exported dict is all it
# takes; the generated manifest.py stays untouched.
_PROVISION_DISABLED = ("nm", "fw", "tsync")
PROCESS_NODES = {
    name: ({**meta, "run_on_start": False} if name in _PROVISION_DISABLED else meta)
    for name, meta in _BASE_PROCESS_NODES.items()
}

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
    # The zone-of-responsibility worker slice. Its name IS its role — "zonal" —
    # because the framework runtime manifest is ROLE-KEYED, not board-named: it
    # declares exactly TWO machines, `master` and `zonal`. colony deploys the
    # `zonal` slice to N worker boards (all role="zonal", all named "zonal" at
    # runtime until a day-2 user SWP renames them); the framework has NO knowledge
    # of any board names. No etcd (reaches the master's shared etcd over TIPC).
    return MachineLayer(name="zonal", role=Explicit("zonal"),
                        arch=Explicit("x86_64"),
                        cores={0, 1}, machine_states={"Startup", "Running"})


# ── RIG — master + the `zonal` worker slice (the framework runtime). ─────────
# EXACTLY two machines: `master` (all singletons + control-plane FCs) and `zonal`
# (the per-board worker set: ucm + shwa). N-arity is a DEPLOY-TIME fact — colony
# fans the one `zonal` slice onto N boards — NOT a manifest fact. The framework
# names no boards; a day-2 user SWP renames the deployed workers as it wishes.
#
# Processes are deduplicated BY NAME in the serializer (last placement wins), so
# an FC that runs on BOTH machines (shwa — per-board HW telemetry) MUST be one
# ProcessLayer with set-valued `machines`, NOT two single-machine layers.
_PER_BOARD   = {"shwa"} & ALL        # runs on master AND zonal (HW telemetry)
_ZONE_ONLY   = ZONAL - _PER_BOARD    # zonal-exclusive (ucm — worker OTA installer)
_MASTER_ONLY = ALL - ZONAL           # everything else → master only
RIG = BASE.combine(DeploymentLayer(
    machines=MachineSetLayer(machines={_master_machine(), _zonal_machine()}),
    execution=ExecutionLayer(processes={
        *(Append(ProcessLayer(name=n, machine=Explicit("master")))
          for n in _MASTER_ONLY),
        # zonal-exclusive FCs (ucm) on the `zonal` machine only.
        *(Append(ProcessLayer(name=n, machine=Explicit("zonal")))
          for n in _ZONE_ONLY),
        # shwa on master AND zonal — set-valued machines (one process, two boards).
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

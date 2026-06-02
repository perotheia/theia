"""tdb client core — probe-backed Theia Debug Bridge clients.

NO raw TIPC. Every peer (supervisor, log[trace]) is reached through
`artheia.probe`, which resolves the peer's TIPC address + per-op service_id
from tools/tdb/system/tdb.art (imported supervisor + log interfaces). If the
Theia transport changes, the runtime + probe change and this file does not.
See feedback-clients-via-art-probe + docs/tasks/TODO/e2e-local-stage-tdb.md.

This is the wiring layer the `tdb` CLI (ps / trace / logcat / supervisor)
sits on. The CLI itself is a follow-up; this proves the client path.

    sup = SupervisorClient.from_workspace(REPO)
    reply = sup.configure_trace(target_node="sm", msg_type="SmStateMsg",
                                enabled=True, kind=0)
    # firehose: sup.events() yields NodeEdge / NodeState as they arrive

    trace = TraceClient.from_workspace(REPO)   # reuses artheia.observer
    for rec in trace.records(timeout=2.0):
        print(rec.src, rec.msg_type)
"""
from __future__ import annotations

from pathlib import Path
from typing import Any, Iterator

# tdb.art lives at the canonical system/ path (symlinked) so its
# `import system.supervisor.* / system.services.log.*` lines resolve.
_ART = "system/tools/tdb/tdb.art"
_PROTO = "platform/proto"


def _ctx(repo: str | Path):
    import sys
    repo = Path(repo)
    sys.path.insert(0, str(repo / "artheia"))
    from artheia.gen_server.probe import ArtheiaContext
    return ArtheiaContext(str(repo / _ART), proto_root=str(repo / _PROTO))


def _unique_instance() -> int:
    """A per-process TIPC source instance so successive/concurrent tdb
    invocations don't race on TdbSup's single .art-declared address while a
    previous process's socket drains. PID, masked to a u32-ish range."""
    import os
    return os.getpid() & 0x7FFFFFFF


class SupervisorClient:
    """Drives SupervisorControlIf from the TdbSup node, via the probe.

    The probe binds TdbSup's TYPE (0x80020101) as the call SOURCE — but at a
    per-process INSTANCE (PID) so back-to-back tdb processes don't collide on
    the fixed instance 0. It resolves SupervisorCtl (0x80020001) + its ops from
    the .art.
    """

    def __init__(self, ctx) -> None:
        self.ctx = ctx
        self.probe = ctx.probe("TdbSup", instance=_unique_instance()).start()

    @classmethod
    def from_workspace(cls, repo: str | Path) -> "SupervisorClient":
        return cls(_ctx(repo))

    # ---- control ops (each a single nanopb CALL over TIPC) ----------------
    def get_tree(self, timeout: float = 2.0) -> dict[str, Any]:
        return self.probe.call("SupervisorCtl", "GetTree", timeout=timeout)

    def get_system_info(self, timeout: float = 2.0) -> dict[str, Any]:
        return self.probe.call("SupervisorCtl", "GetSystemInfo", timeout=timeout)

    def configure_trace(self, *, target_node: str, msg_type: str = "",
                        enabled: bool, kind: int = 0,
                        timeout: float = 2.0) -> dict[str, Any]:
        # ConfigureTrace(req{ config: TraceConfig{ target_node;
        #   platform.runtime.TraceControlPush trace_ctrl } }). msg_type is no
        # longer carried (kept as an accepted-but-ignored kwarg for callers).
        return self.probe.call(
            "SupervisorCtl", "ConfigureTrace", timeout=timeout,
            config=dict(target_node=target_node,
                        trace_ctrl=dict(kind=kind, enabled=enabled)))

    def get_trace_config(self, timeout: float = 2.0) -> dict[str, Any]:
        return self.probe.call("SupervisorCtl", "GetTraceConfig", timeout=timeout)

    _LEVELS = {"trace": 0, "debug": 1, "info": 2, "warn": 3, "error": 4}

    def configure_log_level(self, *, target_node: str, level: str,
                            timeout: float = 2.0) -> dict[str, Any]:
        # level is now the platform.runtime.LogLevelValue enum (ordinal), not a
        # string — map the name to its ordinal.
        lvl = self._LEVELS.get(level.lower(), 2)
        return self.probe.call(
            "SupervisorCtl", "ConfigureLogLevel", timeout=timeout,
            config=dict(target_node=target_node, level=lvl))

    def restart_child(self, name: str, timeout: float = 2.0) -> dict[str, Any]:
        return self.probe.call("SupervisorCtl", "RestartChild", timeout=timeout,
                               name=name, no_restart=False)

    def terminate_hold(self, name: str, timeout: float = 2.0) -> dict[str, Any]:
        # TerminateChild with no_restart=true = stop-and-hold (test mocking).
        return self.probe.call("SupervisorCtl", "TerminateChild", timeout=timeout,
                               name=name, no_restart=True)

    # ---- firehose (SupervisorEventIf: NodeEdge / NodeState / snap_*) -------
    def on_edge(self, handler) -> None:
        self.probe.on_cast("NodeEdge", handler)

    def on_node_state(self, handler) -> None:
        self.probe.on_cast("NodeState", handler)

    def stop(self) -> None:
        self.probe.stop()


class TraceClient:
    """Subscribes to log[trace] and yields decoded records.

    Thin wrapper over artheia.observer.TraceObserver — the SAME probe-backed
    path observer_stream.py exercises. Re-homed here so `tdb trace` / `logcat`
    have one entry point.
    """

    def __init__(self, observer) -> None:
        self.obs = observer

    @classmethod
    def from_workspace(cls, repo: str | Path) -> "TraceClient":
        import sys
        repo = Path(repo)
        sys.path.insert(0, str(repo / "artheia"))
        from artheia.observer.observer import TraceObserver
        obs = TraceObserver.from_log_art(
            str(repo / "system/services/log/component.art"),
            proto_root=str(repo / _PROTO))
        obs.start()
        return cls(obs)

    def records(self, timeout: float = 5.0) -> Iterator:
        yield from self.obs.records(timeout=timeout)

    def stop(self) -> None:
        self.obs.stop()

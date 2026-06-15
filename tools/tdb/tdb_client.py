"""tdb client core — probe-backed Theia Debug Bridge clients.

NO raw TIPC. Every peer (supervisor, log[trace]) is reached through
`artheia.probe`, which resolves the peer's TIPC address + per-op service_id
from tools/tdb/system/tdb.art (imported supervisor + log interfaces). If the
Theia transport changes, the runtime + probe change and this file does not.
See feedback-clients-via-art-probe + docs/tasks/TODO/e2e-local-stage-tdb.md.

This is the wiring layer the `tdb` CLI (ps / trace / tracecat / supervisor)
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

    def __init__(self, ctx, instance: int = 0) -> None:
        self.ctx = ctx
        self.probe = ctx.probe("TdbSup", instance=_unique_instance()).start()
        # The TARGET supervisor. central's SupervisorCtl is instance 0; compute's
        # ComputeSupervisorCtl is the same TYPE at instance 1 (they coexist on one
        # host TIPC namespace). `tdb -i <n>` picks which one. Override the .art ref
        # (instance 0) so the call targets the right supervisor.
        import dataclasses
        self._target = dataclasses.replace(
            ctx.ref("SupervisorCtl"), tipc_instance=instance)
        self.instance = instance

    @classmethod
    def from_workspace(cls, repo: str | Path, instance: int = 0) -> "SupervisorClient":
        return cls(_ctx(repo), instance=instance)

    # ---- control ops (each a single nanopb CALL over TIPC) ----------------
    def get_tree(self, timeout: float = 2.0) -> dict[str, Any]:
        return self.probe.call(self._target, "GetTree", timeout=timeout)

    def get_system_info(self, timeout: float = 2.0) -> dict[str, Any]:
        return self.probe.call(self._target, "GetSystemInfo", timeout=timeout)

    def configure_trace(self, *, target_node: str, msg_type: str = "",
                        enabled: bool, kind: int = 0,
                        timeout: float = 2.0) -> dict[str, Any]:
        # ConfigureTrace(req{ config: TraceConfig{ target_node;
        #   platform.runtime.TraceControlPush trace_ctrl } }). msg_type is no
        # longer carried (kept as an accepted-but-ignored kwarg for callers).
        return self.probe.call(
            self._target, "ConfigureTrace", timeout=timeout,
            config=dict(target_node=target_node,
                        trace_ctrl=dict(kind=kind, enabled=enabled)))

    def get_trace_config(self, timeout: float = 2.0) -> dict[str, Any]:
        return self.probe.call(self._target, "GetTraceConfig", timeout=timeout)

    _LEVELS = {"trace": 0, "debug": 1, "info": 2, "warn": 3, "error": 4}

    def configure_log_level(self, *, target_node: str, level: str,
                            timeout: float = 2.0) -> dict[str, Any]:
        # LogLevelConfig now embeds a LogLevelPush{level} (like TraceConfig
        # embeds TraceControlPush). level name → LogLevelValue ordinal.
        lvl = self._LEVELS.get(level.lower(), 2)
        return self.probe.call(
            self._target, "ConfigureLogLevel", timeout=timeout,
            config=dict(target_node=target_node, log_level=dict(level=lvl)))

    # LogLevelValue ordinal → name (inverse of _LEVELS).
    _LEVEL_NAMES = {0: "trace", 1: "debug", 2: "info", 3: "warn", 4: "error"}

    def get_log_level_config(self, timeout: float = 2.0) -> dict[str, Any]:
        # Every reporting node's effective level (boot ⊕ override).
        return self.probe.call(self._target, "GetLogLevelConfig",
                               timeout=timeout)

    def restart_child(self, name: str, timeout: float = 2.0) -> dict[str, Any]:
        return self.probe.call(self._target, "RestartChild", timeout=timeout,
                               name=name, no_restart=False)

    def terminate_hold(self, name: str, timeout: float = 2.0) -> dict[str, Any]:
        # TerminateChild with no_restart=true = stop-and-hold (test mocking).
        return self.probe.call(self._target, "TerminateChild", timeout=timeout,
                               name=name, no_restart=True)

    # ---- firehose (SupervisorEventIf: NodeEdge / NodeState / snap_*) -------
    def on_edge(self, handler) -> None:
        self.probe.on_cast("NodeEdge", handler)

    def on_node_state(self, handler) -> None:
        self.probe.on_cast("NodeState", handler)

    def stop(self) -> None:
        self.probe.stop()


class PerClient:
    """Drives services/per (config gatekeeper) — PerManager (Snapshot / schema /
    migrate) + PerClient (Get/Put/Watch). Built from tdb.art's TdbPer node so
    the probe resolves per's TIPC addrs + op service_ids straight from the model.
    """

    def __init__(self, ctx, repo) -> None:
        self.ctx = ctx
        self.repo = Path(repo)
        self.probe = ctx.probe("TdbPer", instance=_unique_instance()).start()

    @classmethod
    def from_workspace(cls, repo: str | Path) -> "PerClient":
        repo = Path(repo)
        return cls(_ctx(repo), repo)

    def snapshot(self, label: str, timeout: float = 3.0) -> dict[str, Any]:
        return self.probe.call("PerManager", "Snapshot", timeout=timeout,
                               label=label)

    def restore_snapshot(self, label: str, timeout: float = 3.0) -> dict[str, Any]:
        return self.probe.call("PerManager", "RestoreSnapshot", timeout=timeout,
                               label=label)

    def list_schemas(self, config_type: str = "", timeout: float = 3.0):
        return self.probe.call("PerManager", "ListSchemas", timeout=timeout,
                               config_type=config_type)

    def migrate_bulk(self, config_type: str, from_digest: str, to_digest: str,
                     plugin_so: str = "", timeout: float = 10.0) -> dict[str, Any]:
        return self.probe.call("PerManager", "MigrateBulk", timeout=timeout,
                               config_type=config_type, from_digest=from_digest,
                               to_digest=to_digest, plugin_so=plugin_so)

    # ---- snapshot decode -------------------------------------------------
    def decode_snapshot(self, persnap_path: str | Path,
                        schema_path: str | Path) -> dict:
        """Read a .persnap file + a gen-schema config schema, decode each
        record's opaque config bytes against its config-type proto, and return
        {node: {digest, config_type, config: <decoded dict>}}.

        A record whose stored digest doesn't match any schema config (or whose
        type can't be decoded) is returned with the raw bytes hex + a note, so
        the snapshot is never lost to a single bad record."""
        import json as _json
        recs = _read_persnap(Path(persnap_path))
        schema = _json.loads(Path(schema_path).read_text())
        configs = schema.get("configs", {})
        # node -> config entry (a node binds exactly one config_type).
        node_to_cfg = {}
        for cfg_name, e in configs.items():
            for n in e.get("nodes", []):
                node_to_cfg[n] = (cfg_name, e)

        out = {}
        for node, digest, blob in recs:
            entry = {"digest": digest}
            cfg = node_to_cfg.get(node)
            if cfg is None:
                entry.update(config_type=None, config={"_raw_hex": blob.hex()},
                             note="node not in schema")
            else:
                cfg_name, e = cfg
                entry["config_type"] = cfg_name
                try:
                    entry["config"] = self.ctx.codec.decode(
                        e["art_package"], e["proto_type"], blob)
                except Exception as ex:  # noqa: BLE001
                    entry["config"] = {"_raw_hex": blob.hex()}
                    entry["note"] = f"decode failed: {ex}"
            out[node] = entry
        return out

    def stop(self) -> None:
        self.probe.stop()


def _read_persnap(path: Path):
    """Parse a .persnap file → [(node, digest, config_bytes), ...]. Format
    (snapshot_ops.hpp): "PERSNAP1\\n" then per record three length-prefixed
    (u32 LE) byte fields: node, digest, config."""
    import struct
    data = path.read_bytes()
    magic = b"PERSNAP1\n"
    if not data.startswith(magic):
        raise ValueError(f"{path}: not a per snapshot (bad magic)")
    off = len(magic)

    def field():
        nonlocal off
        (n,) = struct.unpack_from("<I", data, off)
        off += 4
        b = data[off:off + n]
        off += n
        return b

    out = []
    while off < len(data):
        node = field().decode("utf-8", "replace")
        digest = field().decode("utf-8", "replace")
        config = field()
        out.append((node, digest, config))
    return out


class TraceClient:
    """Subscribes to log[trace] and yields decoded records.

    Thin wrapper over artheia.observer.TraceObserver — the SAME probe-backed
    path observer_stream.py exercises. Re-homed here so `tdb trace` / `tracecat`
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


class LogClient:
    """Subscribes to log[logging] and yields decoded log lines.

    The LOG analogue of TraceClient — thin wrapper over
    artheia.observer.LogObserver. Subscribing spins up the log[] tailer; stop()
    unsubscribes (winding it down). `tdb logcat` is the one entry point.
    """

    def __init__(self, observer) -> None:
        self.obs = observer

    @classmethod
    def from_workspace(cls, repo: str | Path) -> "LogClient":
        import sys
        repo = Path(repo)
        sys.path.insert(0, str(repo / "artheia"))
        from artheia.observer.log_observer import LogObserver
        obs = LogObserver.from_log_art(
            str(repo / "system/services/log/component.art"),
            proto_root=str(repo / _PROTO))
        obs.start()
        return cls(obs)

    def records(self, timeout: float = 5.0) -> Iterator:
        yield from self.obs.records(timeout=timeout)

    def stop(self) -> None:
        self.obs.stop()

"""tdb client core — probe-backed Theia Debug Bridge clients.

NO raw TIPC. Every peer (supervisor, log[trace]) is reached through
`artheia.probe`, which resolves the peer's TIPC address + per-op service_id
from system/tools/tdb/tdb.art (imported supervisor + log interfaces). If the
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


def _ctx(repo: str | Path, proto_root: "str | Path | None" = None):
    """Build a probe context. tdb.art resolves under `repo` (the framework
    checkout, which owns system/tools/tdb/tdb.art). proto_root defaults to the
    framework's platform/proto; pass a CONSUMING WORKSPACE's proto/ dir to seed
    or probe an APP config whose .proto lives there — the codec keeps
    $THEIA_ROOT/platform/proto as a fallback root, so platform common types
    (and the tdb.art node protos) still resolve. See the codec include-root list."""
    import os
    import sys
    repo = Path(repo)
    sys.path.insert(0, str(repo / "artheia"))
    from artheia.gen_server.probe import ArtheiaContext
    # Ensure the framework proto is reachable as the codec's fallback root even
    # when proto_root points at a workspace (env.sh exports THEIA_ROOT; default
    # it to `repo` so the fallback is present in a bare invocation).
    os.environ.setdefault("THEIA_ROOT", str(repo))
    root = Path(proto_root) if proto_root else repo / _PROTO
    return ArtheiaContext(str(repo / _ART), proto_root=str(root))


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
    # `machine` is accepted-but-IGNORED: tdb over TIPC targets ONE supervisor
    # (picked by `-i <instance>`), so a machine selector is meaningless here —
    # but the shared command layer (tdb_commands.py) passes machine= for the rtdb
    # path, so accept it to keep one call signature across both clients.
    def get_tree(self, timeout: float = 2.0, machine: str = "") -> dict[str, Any]:
        # PAGINATE: the supervisor's TreeSnapshot.children is a fixed nanopb array
        # (max_count:128); a bigger tree spans pages. Loop offset += the page's
        # row count until last_frame, concatenating children into ONE reply so
        # `tdb ps/apps` see the whole tree. A <=128-row rig is one round-trip.
        first = self.probe.call(self._target, "GetTree", timeout=timeout, offset=0)
        children = list(first.get("children") or [])
        # Robust termination: last_frame set, OR a legacy server (no last_frame /
        # ignores offset) — then the page is the whole (truncated) tree, stop.
        if first.get("last_frame", True):
            return first
        offset = len(children)
        for _ in range(64):        # 64*128 = 8192 rows — runaway backstop
            page = self.probe.call(self._target, "GetTree",
                                   timeout=timeout, offset=offset)
            pc = list(page.get("children") or [])
            if not pc:
                break
            children.extend(pc)
            offset += len(pc)
            if page.get("last_frame", True):
                break
        first["children"] = children
        first["truncated"] = False           # fully paged → nothing dropped
        first["total_children"] = len(children)
        return first

    def get_system_info(self, timeout: float = 2.0,
                        machine: str = "") -> dict[str, Any]:
        return self.probe.call(self._target, "GetSystemInfo", timeout=timeout)

    def configure_trace(self, *, target_node: str, msg_type: str = "",
                        enabled: bool, kind: int = 0,
                        instances: "list[int] | None" = None,
                        timeout: float = 2.0) -> dict[str, Any]:
        # ConfigureTrace(req{ config: TraceConfig{ target_node;
        #   platform.runtime.TraceControlPush trace_ctrl; instances[] } }). msg_type
        # is no longer carried (accepted-but-ignored). instances[] (optional) targets
        # specific CLONES of the node's TIPC type; empty = the node's single address.
        cfg = dict(target_node=target_node,
                   trace_ctrl=dict(kind=kind, enabled=enabled))
        if instances:
            cfg["instances"] = list(instances)
        return self.probe.call(
            self._target, "ConfigureTrace", timeout=timeout, config=cfg)

    def get_trace_config(self, timeout: float = 2.0) -> dict[str, Any]:
        return self.probe.call(self._target, "GetTraceConfig", timeout=timeout)

    _LEVELS = {"trace": 0, "debug": 1, "info": 2, "warn": 3, "error": 4}

    def configure_log_level(self, *, target_node: str, level: str,
                            instances: "list[int] | None" = None,
                            timeout: float = 2.0) -> dict[str, Any]:
        # LogLevelConfig now embeds a LogLevelPush{level} (like TraceConfig
        # embeds TraceControlPush). level name → LogLevelValue ordinal. instances[]
        # (optional) targets specific clones; empty = the node's single address.
        lvl = self._LEVELS.get(level.lower(), 2)
        cfg = dict(target_node=target_node, log_level=dict(level=lvl))
        if instances:
            cfg["instances"] = list(instances)
        return self.probe.call(
            self._target, "ConfigureLogLevel", timeout=timeout, config=cfg)

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
    def from_workspace(cls, repo: str | Path,
                       app_proto: "str | Path | None" = None) -> "PerClient":
        """repo = the framework checkout (owns tdb.art). app_proto = a CONSUMING
        WORKSPACE's proto/ dir, when driving an APP config whose .proto lives
        there (seed.py --workspace); the codec keeps the framework platform/proto
        as a fallback root so platform + tdb node types still resolve. Omit
        app_proto for framework-only configs (services)."""
        repo = Path(repo)
        return cls(_ctx(repo, proto_root=app_proto), repo)

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
        {node: {digest, config_type, config: <decoded dict>}}. A per-instance
        key ("<component>/<instance>") resolves its config-type via the base
        component and carries an extra "instance" field.

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
            # Per-INSTANCE key: a cloned node stores its config under
            # "<component>/<instance>" (counter/1 — the instance may also be
            # machine-shifted on a multi-machine rig). The schema is keyed by
            # NODE, so resolve the config-type against the base component and
            # surface the parsed instance in the entry.
            base, sep, tail = node.rpartition("/")
            if sep and tail.isdigit():
                entry["instance"] = int(tail)
            else:
                base = node
            cfg = node_to_cfg.get(node) or node_to_cfg.get(base)
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


class NmClient:
    """Drives services/nm (network management) — NmDaemon's NmCtlIf. Built from
    tdb.art's TdbNm node so the probe resolves nm's TIPC addr + op service_ids
    from the model. `wifi_scan` returns the visible AP list + association
    snapshot; `net_status` the readiness state."""

    def __init__(self, ctx, repo) -> None:
        self.ctx = ctx
        self.repo = Path(repo)
        self.probe = ctx.probe("TdbNm", instance=_unique_instance()).start()

    @classmethod
    def from_workspace(cls, repo: str | Path) -> "NmClient":
        repo = Path(repo)
        return cls(_ctx(repo), repo)

    def wifi_scan(self, interface: str = "", timeout: float = 8.0) -> dict[str, Any]:
        # An active scan can take several seconds — give it a generous budget.
        return self.probe.call("NmDaemon", "WifiScan", timeout=timeout,
                               interface=interface)

    def net_status(self, timeout: float = 3.0) -> dict[str, Any]:
        return self.probe.call("NmDaemon", "GetNetworkStatus", timeout=timeout)

    # ---- config-transaction ops (NmCfgGate) — enroll wifi + toggle VPN. Each
    # returns NmCfgReply{ok, message, profiles, txn_state}; txn_state==2 (PENDING)
    # means "applied, now `confirm` over the new path within the window". ----
    def add_wifi(self, ssid: str, psk: str = "", priority: int = 0,
                 timeout: float = 4.0) -> dict[str, Any]:
        return self.probe.call("NmCfgGate", "AddWifi", timeout=timeout,
                               ssid=ssid, psk=psk, priority=priority)

    def remove_wifi(self, ssid: str, timeout: float = 4.0) -> dict[str, Any]:
        return self.probe.call("NmCfgGate", "RemoveWifi", timeout=timeout, ssid=ssid)

    def set_vpn(self, require_vpn: bool, auto_vpn: bool,
                timeout: float = 4.0) -> dict[str, Any]:
        return self.probe.call("NmCfgGate", "SetVpn", timeout=timeout,
                               require_vpn=require_vpn, auto_vpn=auto_vpn)

    def set_autoconnect(self, auto_connect: bool,
                        timeout: float = 4.0) -> dict[str, Any]:
        return self.probe.call("NmCfgGate", "SetAutoConnect", timeout=timeout,
                               auto_connect=auto_connect)

    def confirm_config(self, timeout: float = 4.0) -> dict[str, Any]:
        return self.probe.call("NmCfgGate", "ConfirmConfig", timeout=timeout)

    def abort_config(self, timeout: float = 4.0) -> dict[str, Any]:
        return self.probe.call("NmCfgGate", "AbortConfig", timeout=timeout)

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

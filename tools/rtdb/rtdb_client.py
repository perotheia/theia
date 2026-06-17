#!/usr/bin/env python3
"""rtdb_client — gRPC-backed clients with the SAME method surface tdb_client's
TIPC-backed ones expose, so rtdb reuses tdb's shared command + render layer
(tools/tdb/tdb_commands) verbatim.

SupervisorClient drives services/com's SupervisorView gRPC (the "tdb over gRPC"
proxy): control RPCs + the GetTree-poll live tree (Subscribe). Its methods
return the protobuf reply objects directly — tdb_commands._g reads fields off a
dict OR a protobuf message via getattr, so no dict conversion is needed.

TraceClient drives the collector's TraceStream gRPC (tracecat). It wraps each
gRPC TraceRecord in a small adapter exposing the same attrs tdb's
artheia.observer records do (src/kind/msg_type/corr_id/ts_ns/content/to_dict),
so the shared cmd_tracecat renders identically.
"""
from __future__ import annotations

import os
import sys
from pathlib import Path
from typing import Any, Iterator

import grpc


def _default_certs_dir() -> "Path | None":
    """In-source dev default for the mTLS material: the certs `theia manifest`
    stages under dist/manifest/<machine>/certs/. So `rtdb ps` Just Works against
    the local mTLS cluster without exporting THEIA_COM_TLS_* by hand.

    Resolves $THEIA_ROOT (env.sh exports it) else this file's workspace root,
    then dist/manifest/<machine>/certs — preferring 'central', else the first
    machine dir that has a ca.crt. Returns None if no dev certs exist (a plain
    insecure local stack)."""
    root = os.environ.get("THEIA_ROOT", "")
    base = Path(root) if root else Path(__file__).resolve().parents[2]
    mdir = base / "dist" / "manifest"
    if not mdir.is_dir():
        return None
    for name in ("central", *sorted(p.name for p in mdir.iterdir() if p.is_dir())):
        d = mdir / name / "certs"
        if (d / "ca.crt").is_file():
            return d
    return None


def _make_channel(target: str) -> "grpc.Channel":
    """gRPC channel to com, TLS or insecure (matches com's opt-in TLS).

    TLS is OPT-IN, the same way com flips it: when a CA is available, connect
    with SSL channel creds that trust it; otherwise stay insecure. The CA +
    client cert/key come from THEIA_COM_TLS_CA / _CLIENT_CERT / _CLIENT_KEY when
    set; OTHERWISE they DEFAULT to the in-source dev certs under
    dist/manifest/<machine>/certs (so in-source `rtdb ps` needs no env). com's
    mutual-TLS policy REQUIRES the client cert when a CA is pinned, so we present
    it whenever the dir has one."""
    certs = _default_certs_dir()
    ca_path = os.environ.get("THEIA_COM_TLS_CA") \
        or (str(certs / "ca.crt") if certs else "")
    if not ca_path or not os.path.isfile(ca_path):
        return grpc.insecure_channel(target)
    with open(ca_path, "rb") as f:
        ca = f.read()
    client_cert = client_key = None
    cc = os.environ.get("THEIA_COM_TLS_CLIENT_CERT") \
        or (str(certs / "client.crt") if certs else "")
    ck = os.environ.get("THEIA_COM_TLS_CLIENT_KEY") \
        or (str(certs / "client.key") if certs else "")
    if cc and ck and os.path.isfile(cc) and os.path.isfile(ck):
        with open(cc, "rb") as f:
            client_cert = f.read()
        with open(ck, "rb") as f:
            client_key = f.read()
    creds = grpc.ssl_channel_credentials(
        root_certificates=ca,
        private_key=client_key, certificate_chain=client_cert)
    return grpc.secure_channel(target, creds)

# The grpc_tools-generated stubs use flat-style imports (import Foo_pb2), so
# their dir must be on sys.path before we import them. Regenerate with
# tools/rtdb/gen_protos.sh.
_GEN = Path(__file__).resolve().parent / "_gen"
sys.path.insert(0, str(_GEN))

import supervisor_bridge_pb2 as _br          # noqa: E402
import supervisor_bridge_pb2_grpc as _brg    # noqa: E402
import supervisor_pb2 as _sup                # noqa: E402,F401

# TraceStream is optional (the trace EGRESS gRPC; moves into com later).
_DEFAULT_TARGET = "127.0.0.1:7700"
# com's TraceForwarder gRPC endpoint (Phase C: the trace egress moved INTO com,
# served by the TraceForwarder runnable — TraceStream now lives in
# supervisor_bridge.proto, NOT the retired services/log trace_stream.proto).
# Distinct port from the SupervisorView :7700 so the two runnables are
# independently restartable.
_DEFAULT_COLLECTOR = "127.0.0.1:7710"
# com's LogForwarder gRPC endpoint (the LOG egress, the logcat analogue of the
# trace collector). Distinct port from SupervisorView :7700 and TraceForwarder
# :7710 so the three com runnables are independently restartable.
_DEFAULT_LOG_COLLECTOR = "127.0.0.1:7711"

_LEVELS = {"trace": 0, "debug": 1, "info": 2, "warn": 3, "error": 4}
_KIND_NAMES = {0: "OTHER", 1: "CAST_OUT", 2: "CAST_IN",
               3: "CALL_OUT", 4: "CALL_IN", 5: "STATEM"}


class SupervisorClient:
    """gRPC mirror of tdb_client.SupervisorClient. Drop-in for the shared
    cmd_* functions: same method names, same kwargs, returns protobuf replies
    (read via tdb_commands._g)."""

    def __init__(self, target: str = _DEFAULT_TARGET) -> None:
        self._target = target
        self._channel = _make_channel(target)
        self._stub = _brg.SupervisorViewStub(self._channel)

    # tdb_client parity: from_workspace(repo, instance). rtdb is transport-only
    # (no workspace .art), so repo is ignored; instance has no meaning over a
    # single com endpoint (one com per machine) — accepted + ignored.
    @classmethod
    def from_workspace(cls, repo: Any = None, instance: int = 0,
                       target: str = _DEFAULT_TARGET) -> "SupervisorClient":
        return cls(target)

    # ---- live tree --------------------------------------------------------
    def get_tree(self, timeout: float = 4.0) -> Any:
        """One TreeSnapshot. com has no unary GetTree rpc — the live tree is the
        Subscribe poll-stream (the supervisor's event firehose has no remote
        egress). Pull a snapshot and return it (cmd_ps reads .children).
        cmd_ps --follow just calls this on an interval, same as tdb.

        SKIP empty snapshots: com's FIRST poll tick after a (re)connect can emit
        an empty/partial TreeSnapshot before its SupLink GetTree to the
        supervisor warms (or right at a subscriber attach boundary). A one-shot
        `rtdb ps` that returned that first frame printed "(empty tree)" for a live
        cluster. Wait (within the timeout) for the first snapshot that actually
        has children; fall back to the last (possibly empty) one we saw."""
        stream = self._stub.Subscribe(_br.SubscribeRequest(), timeout=timeout)
        last = _sup.TreeSnapshot()
        try:
            for obs in stream:
                if obs.WhichOneof("kind") == "snapshot":
                    last = obs.snapshot
                    if len(last.children) > 0:
                        return last        # first non-empty wins
        except grpc.RpcError:
            pass
        finally:
            stream.cancel()
        return last                        # empty only if nothing arrived in time

    # ---- host facts -------------------------------------------------------
    def get_system_info(self, timeout: float = 2.0) -> Any:
        return self._stub.GetSystemInfo(_br.GetSystemInfoCall(), timeout=timeout)

    # ---- trace ------------------------------------------------------------
    def configure_trace(self, *, target_node: str, msg_type: str = "",
                        enabled: bool, kind: int = 0,
                        timeout: float = 2.0) -> Any:
        return self._stub.ConfigureTrace(
            _br.TraceConfigRequest(target_node=target_node, msg_type=msg_type,
                                   enabled=enabled, kind=kind),
            timeout=timeout)

    def get_trace_config(self, timeout: float = 2.0) -> Any:
        return self._stub.GetTraceConfig(_br.GetTraceConfigCall(), timeout=timeout)

    # ---- log level --------------------------------------------------------
    def configure_log_level(self, *, target_node: str, level: str,
                            timeout: float = 2.0) -> Any:
        # com's LogLevelCall carries the level by NAME (the gRPC edge maps it to
        # the LogLevelValue ordinal in sup_link). Pass the name straight through.
        return self._stub.ConfigureLogLevel(
            _br.LogLevelCall(target_node=target_node, level=level.lower()),
            timeout=timeout)

    def get_log_level_config(self, timeout: float = 2.0) -> Any:
        return self._stub.GetLogLevelConfig(_br.GetLogLevelConfigCall(),
                                            timeout=timeout)

    # ---- child lifecycle --------------------------------------------------
    def restart_child(self, name: str, timeout: float = 2.0) -> Any:
        return self._stub.RestartChild(
            _sup.ChildSelector(name=name, no_restart=False), timeout=timeout)

    def terminate_hold(self, name: str, timeout: float = 2.0) -> Any:
        return self._stub.TerminateChild(
            _sup.ChildSelector(name=name, no_restart=True), timeout=timeout)

    def stop(self) -> None:
        self._channel.close()


class PerClient:
    """gRPC client for com's PerView service — proxies services/per's manager
    ops (ListSchemas / Snapshot) the SAME way the GUI's Persistency panel does.
    On the SAME :7700 endpoint as SupervisorView."""

    def __init__(self, target: str = _DEFAULT_TARGET) -> None:
        self._channel = _make_channel(target)
        self._stub = _brg.PerViewStub(self._channel)

    def list_schemas(self, config_type: str = "", timeout: float = 3.0):
        rep = self._stub.ListSchemas(
            _br.ListSchemasCall(config_type=config_type), timeout=timeout)
        return [(s.config_type, s.digest) for s in rep.schemas]

    def snapshot(self, label: str, timeout: float = 5.0):
        rep = self._stub.Snapshot(_br.SnapshotCall(label=label), timeout=timeout)
        return rep.status, rep.message, rep.mod_rev

    def stop(self) -> None:
        self._channel.close()


# ---------------------------------------------------------------------------
# trace (tracecat) — gRPC TraceStream, adapted to tdb's record shape
# ---------------------------------------------------------------------------

class _RecordView:
    """Wraps a gRPC TraceRecord so the shared cmd_tracecat sees the SAME attrs
    artheia.observer records expose. content is the decoded inner message dict
    when a decoder is available, else None (cmd_tracecat prints raw header only)."""

    def __init__(self, rec, content=None, data=None) -> None:
        self.ts_ns    = rec.ts_ns
        self.src      = rec.node_name        # tdb names this `src`
        self.dst      = rec.dst
        self.msg_type = rec.msg_type
        self.corr_id  = rec.corr_id
        self.kind     = _KIND_NAMES.get(rec.kind, str(rec.kind))
        self.payload  = rec.payload
        self.content  = content
        # STATEM-only transition states (gRPC fields 8/9); "" on other kinds
        # and on an older collector that predates the proto extension.
        self.from_state = getattr(rec, "from_state", "") or ""
        self.to_state   = getattr(rec, "to_state", "") or ""
        # STATEM FSM data (OTP Data term): field 10 type name + decoded dict.
        self.data_type  = getattr(rec, "data_type", "") or ""
        self.data       = data

    def to_dict(self, ts: str = "") -> dict:
        d = {
            "ts": ts,
            "ts_ns": self.ts_ns,
            "src": self.src,
            "dst": self.dst,
            "kind": self.kind,
            "msg_type": self.msg_type,
            "corr_id": self.corr_id,
        }
        if self.to_state:
            d["from_state"] = self.from_state
            d["to_state"] = self.to_state
        if self.data is not None:
            d["data"] = self.data
        if self.content is not None:
            d["content"] = self.content
        else:
            d["payload_len"] = len(self.payload)
        return d


class TraceClient:
    """gRPC TraceStream subscriber, yielding _RecordView (tdb record shape).

    Optional payload decode via rf_theia's libtrace_decoder.so (same FFI tdb's
    observer uses); falls back to raw (content=None) when unavailable."""

    def __init__(self, target: str = _DEFAULT_COLLECTOR, *, kind: int = 0,
                 node: str = "", decode: bool = True) -> None:
        # TraceStream lives in supervisor_bridge (com's TraceForwarder).
        self._channel = _make_channel(target)
        self._stub = _brg.TraceStreamStub(self._channel)
        self._kind = kind
        self._node = node
        self._decoder = None
        if decode:
            try:
                from rf_theia.adapters.trace_decoder import open_default
                self._decoder = open_default()
            except Exception:
                self._decoder = None       # raw fallback

    @classmethod
    def from_workspace(cls, repo: Any = None,
                       target: str = _DEFAULT_COLLECTOR) -> "TraceClient":
        return cls(target)

    def records(self, timeout: float = 5.0) -> Iterator[_RecordView]:
        req = _br.TraceSubscribeRequest(kind=self._kind, target_node=self._node)
        for rec in self._stub.Subscribe(req, timeout=timeout):
            content = None
            data = None
            if self._decoder is not None and rec.payload:
                # STATEM rows carry the FSM `data` message (decode by data_type
                # → `data`); every other row's payload is the traced message
                # itself (decode by msg_type → `content`).
                dtype = getattr(rec, "data_type", "") or ""
                try:
                    if dtype:
                        data = self._decoder.decode(dtype, rec.payload)
                    else:
                        content = self._decoder.decode(rec.msg_type, rec.payload)
                except Exception:
                    content = data = None
            yield _RecordView(rec, content, data)

    def stop(self) -> None:
        self._channel.close()


# ---------------------------------------------------------------------------
# log (logcat) — gRPC LogStream, adapted to tdb's LogRec shape
# ---------------------------------------------------------------------------

# LogLevel ordinal → single-letter code + name (mirrors
# artheia.observer.LEVEL_CODES so rtdb logcat renders identically to tdb).
_LOG_LEVEL_CODES = ["V", "D", "I", "W", "E", "F"]
_LOG_LEVEL_NAMES = ["VERBOSE", "DEBUG", "INFO", "WARNING", "ERROR", "FATAL"]


class _LogRecordView:
    """Wraps a gRPC LogRecord so the shared cmd_logcat sees the SAME attrs
    artheia.observer.LogRec exposes (node/tag/level/level_ord/ts_ns/line)."""

    def __init__(self, rec) -> None:
        self.node      = rec.node
        self.tag       = rec.tag
        self.level_ord = int(rec.level)
        self.level     = (_LOG_LEVEL_NAMES[self.level_ord]
                          if 0 <= self.level_ord < len(_LOG_LEVEL_NAMES) else "")
        self.ts_ns     = rec.ts_ns
        self.line      = rec.line

    @property
    def level_code(self) -> str:
        return (_LOG_LEVEL_CODES[self.level_ord]
                if 0 <= self.level_ord < len(_LOG_LEVEL_CODES) else "?")

    def to_dict(self, ts: str = "") -> dict:
        return {
            "ts": ts,
            "node": self.node,
            "tag": self.tag,
            "level": self.level,
            "line": self.line,
        }


class LogClient:
    """gRPC LogStream subscriber, yielding _LogRecordView (tdb LogRec shape).

    The rtdb analogue of LogObserver — drives com's LogForwarder LogStream
    (:7711). Subscribing spins up the log[] tailer; stop() (channel close) winds
    it down. The <tag-glob>:<level> filter is applied client-side by cmd_logcat,
    exactly as on the tdb (TIPC) path."""

    def __init__(self, target: str = _DEFAULT_LOG_COLLECTOR, *,
                 level_min: int = 0, tag: str = "") -> None:
        # LogStream lives in supervisor_bridge (com's LogForwarder runnable).
        self._channel = _make_channel(target)
        self._stub = _brg.LogStreamStub(self._channel)
        self._level_min = level_min
        self._tag = tag

    @classmethod
    def from_workspace(cls, repo: Any = None,
                       target: str = _DEFAULT_LOG_COLLECTOR) -> "LogClient":
        return cls(target)

    def records(self, timeout: float = 5.0) -> Iterator[_LogRecordView]:
        req = _br.LogSubscribeRequest(level_min=self._level_min,
                                      tag_filter=self._tag)
        for rec in self._stub.Subscribe(req, timeout=timeout):
            yield _LogRecordView(rec)

    def stop(self) -> None:
        self._channel.close()

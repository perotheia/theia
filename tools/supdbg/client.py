# supdbg.client — programmatic client over services/com gRPC bridge.
#
# Wraps the generated SupervisorViewStub with a Pythonic API:
#
#   c = Client("127.0.0.1:7700")
#   tree = c.tree()                  # latest TreeSnapshot
#   c.restart_child("demo_p1")       # ControlReply
#   for obs in c.subscribe():        # generator over the firehose
#       print(obs)
#
# Tests can:
#   - drive the supervisor (restart_child, terminate_child, …)
#   - assert on observations (e.g. EXIT events with expected exit_code)
# without spinning up the wx GUI.

from __future__ import annotations

import os
import sys
import time
import dataclasses
import enum
import typing as t

# The generated _gen/ uses flat-style imports (`import ChildState_pb2`)
# because grpc_tools.protoc 1.x can't emit package-relative imports
# when --proto_path covers multiple dirs. Add _gen to sys.path so
# those resolve.
_GEN_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_gen")
if _GEN_DIR not in sys.path:
    sys.path.insert(0, _GEN_DIR)

import grpc  # noqa: E402

import supervisor_bridge_pb2 as _bridge_pb  # noqa: E402
import supervisor_bridge_pb2_grpc as _bridge_grpc  # noqa: E402
import TreeSnapshot_pb2 as _tree_pb  # noqa: E402, F401
import SupervisionEvent_pb2 as _event_pb  # noqa: E402, F401
import HealthBeacon_pb2 as _health_pb  # noqa: E402, F401
import ChildSelector_pb2 as _sel_pb  # noqa: E402
import ChildSpec_pb2 as _spec_pb  # noqa: E402
import ChildState_pb2 as _state_pb  # noqa: E402, F401


# ---------------------------------------------------------------------
# Lightweight value types — easier to assert on in tests than proto
# messages, but the raw proto stays available on .raw for the cases
# where the caller wants every field.
# ---------------------------------------------------------------------

class EventKind(enum.IntEnum):
    # Numeric values come from the .art enum in
    # platform/supervisor/system/package.art (see SupervisionEvent.Kind).
    # We mirror them here so tests don't import the proto enum.
    UNKNOWN     = 0
    STARTED     = 1
    EXITED      = 2
    RESTARTED   = 3
    TERMINATED  = 4
    SUP_STARTED = 5
    SUP_EXITED  = 6


@dataclasses.dataclass
class ChildStateView:
    name: str
    parent: str
    pid: int
    state: int
    restart_count: int
    uptime_ms: int
    cpu_pct: int
    rss_kb: int
    threads: int
    raw: _state_pb.ChildState

    @classmethod
    def _from_pb(cls, m: _state_pb.ChildState) -> "ChildStateView":
        return cls(
            name=m.name,
            parent=m.parent_name,
            pid=m.pid,
            state=m.state,
            restart_count=m.restart_count,
            uptime_ms=m.uptime_ms,
            cpu_pct=m.cpu_pct,
            rss_kb=m.rss_kb,
            threads=m.threads,
            raw=m,
        )


@dataclasses.dataclass
class Observation:
    """One streamed message — exactly one of (event, health, snapshot)
    is non-None, matching the wire-side oneof. Use kind() to dispatch.
    """
    machine: str
    event:    t.Optional[_event_pb.SupervisionEvent] = None
    health:   t.Optional[_health_pb.HealthBeacon]    = None
    snapshot: t.Optional[_tree_pb.TreeSnapshot]      = None

    def kind(self) -> str:
        if self.event    is not None: return "event"
        if self.health   is not None: return "health"
        if self.snapshot is not None: return "snapshot"
        return "empty"


# ---------------------------------------------------------------------
# Client
# ---------------------------------------------------------------------

class Client:
    """One client per machine. Thread-safe for unary RPCs; subscribe()
    returns a generator that may only be iterated from one thread.

    The connection is lazy — the channel opens on first RPC and stays
    open until close()/context-manager exit.
    """

    def __init__(self, host_port: str, machine: t.Optional[str] = None,
                 timeout: float = 15.0) -> None:
        # Default 15s — restart/terminate go through the supervisor's
        # synchronous control path which waits out the child's SIGTERM
        # grace (often 5s). Anything tighter than that would time out
        # on legitimate slow restarts.
        self.host_port = host_port
        self.machine   = machine or host_port
        self.timeout   = timeout
        self._channel: t.Optional[grpc.Channel] = None
        self._stub:    t.Optional[_bridge_grpc.SupervisorViewStub] = None

    # ----- lifecycle --------------------------------------------------

    def __enter__(self) -> "Client":
        self._ensure()
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def _ensure(self) -> _bridge_grpc.SupervisorViewStub:
        if self._stub is None:
            self._channel = grpc.insecure_channel(self.host_port)
            self._stub    = _bridge_grpc.SupervisorViewStub(self._channel)
        return self._stub

    def _ensure_trace(self) -> _bridge_grpc.TraceStreamStub:
        """Lazy TraceStream stub — shares the channel with SupervisorView."""
        self._ensure()
        if not hasattr(self, "_trace_stub") or self._trace_stub is None:
            self._trace_stub = _bridge_grpc.TraceStreamStub(self._channel)
        return self._trace_stub

    def close(self) -> None:
        if self._channel is not None:
            self._channel.close()
            self._channel = None
            self._stub    = None

    def wait_ready(self, timeout: float = 5.0) -> bool:
        """Block until the channel is READY. Returns True on success,
        False on timeout. Useful in tests that spin up the supervisor
        in a subprocess and then connect."""
        if self._channel is None:
            self._ensure()
        assert self._channel is not None
        try:
            grpc.channel_ready_future(self._channel).result(timeout=timeout)
            return True
        except grpc.FutureTimeoutError:
            return False

    # ----- unary control RPCs ----------------------------------------
    # All four mutators return the ControlReply verbatim. status==0 is
    # OK; non-zero means the supervisor refused (e.g. unknown child).

    def start_child(self, spec: _spec_pb.ChildSpec) -> _bridge_pb.ControlReply:
        return self._ensure().StartChild(
            _bridge_pb.StartChildCall(spec=spec),
            timeout=self.timeout,
        )

    def delete_child(self, name: str) -> _bridge_pb.ControlReply:
        return self._ensure().DeleteChild(
            _bridge_pb.DeleteChildCall(name=name),
            timeout=self.timeout,
        )

    def restart_child(self, name: str) -> _bridge_pb.ControlReply:
        return self._ensure().RestartChild(
            _sel_pb.ChildSelector(name=name),
            timeout=self.timeout,
        )

    def terminate_child(self, name: str) -> _bridge_pb.ControlReply:
        return self._ensure().TerminateChild(
            _sel_pb.ChildSelector(name=name),
            timeout=self.timeout,
        )

    # ----- observation -----------------------------------------------

    def subscribe(self) -> t.Iterator[Observation]:
        """Yield Observation objects forever — until the server closes
        the stream or the caller breaks out of the loop.

        Translates the oneof into a tagged dataclass; tests can match
        on `.kind()` without importing proto enums.
        """
        stub = self._ensure()
        stream = stub.Subscribe(_bridge_pb.SubscribeRequest())
        for msg in stream:
            kind = msg.WhichOneof("kind")
            if kind == "event":
                yield Observation(machine=self.machine, event=msg.event)
            elif kind == "health":
                yield Observation(machine=self.machine, health=msg.health)
            elif kind == "snapshot":
                yield Observation(machine=self.machine, snapshot=msg.snapshot)
            else:
                yield Observation(machine=self.machine)

    def tree(self, timeout: float = 3.0) -> t.Optional[_tree_pb.TreeSnapshot]:
        """Return the next TreeSnapshot on the subscribe stream.

        The supervisor emits one snapshot roughly per second, so this
        usually returns within ~1s. Returns None on timeout.

        Convenience for one-shot uses; for sustained observation use
        subscribe().
        """
        deadline = time.monotonic() + timeout
        for obs in self.subscribe():
            if obs.snapshot is not None:
                return obs.snapshot
            if time.monotonic() > deadline:
                return None
        return None  # pragma: no cover — stream exhausted

    # ----- composite helpers (testing-oriented) ----------------------

    def wait_event(
        self,
        kind: t.Optional[EventKind] = None,
        child_name: t.Optional[str] = None,
        timeout: float = 5.0,
    ) -> t.Optional[_event_pb.SupervisionEvent]:
        """Block until a SupervisionEvent matching kind+child_name
        arrives, or timeout expires. Returns None on timeout.

        Useful in tests:
            ev = c.wait_event(EventKind.EXITED, "demo_p1", timeout=2)
            assert ev and ev.exit_code != 0
        """
        deadline = time.monotonic() + timeout
        for obs in self.subscribe():
            if obs.event is not None:
                ev = obs.event
                if (kind is None or ev.kind == kind) and \
                   (child_name is None or ev.child_name == child_name):
                    return ev
            if time.monotonic() > deadline:
                return None
        return None  # pragma: no cover

    # ----- TraceStream (#360) ---------------------------------------------

    def configure_trace(self, target_node: str, msg_type: str,
                        enabled: bool) -> _bridge_pb.ControlReply:
        """Toggle the per-(node, msg_type) filter on the supervisor.

        The supervisor stores the entry and pushes to the worker's
        NodeTraceCtl TIPC port (#361). Config persists across child
        restart — the next heartbeat-after-gap re-fires the push.
        """
        return self._ensure_trace().Configure(
            _bridge_pb.TraceConfigRequest(
                target_node=target_node,
                msg_type=msg_type,
                enabled=enabled,
            ),
            timeout=self.timeout,
        )

    # ----- log level (#385) -----------------------------------------------

    def configure_log_level(self, target_node: str,
                            level: str) -> _bridge_pb.ControlReply:
        """Set a child's runtime log level via the supervisor.

        Routes supdbg → com → supervisor (op_kind=11): the supervisor
        stores the level keyed by child NAME (overwrites the child's
        spawn env THEIA_LOG_LEVEL so a restart re-applies) and pushes a
        live LogLevelConfig frame to the node for no-restart effect.
        `level` is one of trace|debug|info|warn|error.
        """
        return self._ensure().ConfigureLogLevel(
            _bridge_pb.LogLevelCall(
                target_node=target_node,
                level=level,
            ),
            timeout=self.timeout,
        )

    def subscribe_traces(
        self,
        decoder: t.Optional["TraceDecoder"] = None,
    ) -> t.Iterator[t.Tuple[_bridge_pb.TraceRecord, t.Optional[dict]]]:
        """Yield (TraceRecord, decoded_dict-or-None) pairs forever.

        If `decoder` is supplied (typically rf_theia.adapters.trace_
        _decoder.open_default() — the libtrace_decoder.so loaded via
        ctypes per #356), the payload bytes are decoded to a Python
        dict and returned as the second tuple element. Without a
        decoder, decoded_dict is None and the caller works with the
        raw record.
        """
        stream = self._ensure_trace().Subscribe(
            _bridge_pb.TraceSubscribeRequest()
        )
        for rec in stream:
            decoded: t.Optional[dict] = None
            if decoder is not None and rec.payload:
                try:
                    decoded = decoder.decode(rec.msg_type, rec.payload)
                except Exception:
                    # Unknown type / parse failure → surface the raw
                    # record and let the caller decide.
                    decoded = None
            yield rec, decoded

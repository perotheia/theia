"""TheiaTestLibrary — single keyword library for the rf-theia harness.

Keyword families are grouped by prefix so a Robot scenario sees them as one
library import:

  - ``T Sup ...`` — supervisor gRPC control + assertions (phase 1.2 wires).
  - ``T Sig ...`` — signal-flow stimuli + trace assertions (phase 1.3/1.4).
  - ``T Art ...`` — artheia generator regression (phase 2).
  - ``T Prov ...`` — provisioning / Puppet dry-run (phase 3).
  - ``T Orch ...`` — two-phase orchestration (phase 3).

The TPT engine is composed in (not subclassed) — it carries over verbatim
from up/rf_tpt_ls/ and exposes its own ``Create Partition`` etc. keywords
via library composition.

Adapter imports are lazy: the library can be imported on a machine without
grpc tooling installed; the adapter methods raise a clear error on first
use instead. This keeps `robot --dryrun` against the selftest suite
working in any environment.
"""
from __future__ import annotations

import logging
from typing import Any

from robot.api.deco import keyword, library

from .tpt_engine import (
    EventStore,
    Partition,
    RampStimulus,
    SignalStore,
    TimeEngine,
    event_guard,
    signal_guard,
    time_guard,
)

logger = logging.getLogger("rf_theia")


@library(scope="SUITE")
class TheiaTestLibrary:
    """Single-library entry point for rf-theia scenarios.

    Robot scenarios import this with::

        Library    rf_theia.TheiaTestLibrary
    """

    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self) -> None:
        # TPT engine — vendored, domain-agnostic. Drives time-shaped stimuli
        # for T Sig scenarios.
        self.signals = SignalStore()
        self.events = EventStore()
        self.engine = TimeEngine()

        # Adapter handles. Lazily constructed on first keyword call so
        # importing the library never reaches out to the network or fails
        # because grpcio isn't installed.
        self._sup: Any = None
        self._trace: Any = None

    # ──────────────────────────────────────────────────────────────────
    # T Sup — supervisor gRPC. Phase 1.2 fleshes these out.
    # ──────────────────────────────────────────────────────────────────

    @keyword("T Sup Connect")
    def t_sup_connect(self, endpoint: str = "localhost:5051") -> None:
        from .adapters.supervisor_grpc import SupervisorClient
        self._sup = SupervisorClient(endpoint)
        self._sup.connect()
        logger.info("T Sup: connected to %s", endpoint)

    @keyword("T Sup Disconnect")
    def t_sup_disconnect(self) -> None:
        if self._sup is not None:
            self._sup.close()
            self._sup = None

    @keyword("T Sup Start Child")
    def t_sup_start_child(self, name: str) -> None:
        self._require_sup()
        self._sup.start_child(name)

    @keyword("T Sup Restart Child")
    def t_sup_restart_child(self, name: str) -> None:
        self._require_sup()
        self._sup.restart_child(name)

    @keyword("T Sup Terminate Child")
    def t_sup_terminate_child(self, name: str) -> None:
        self._require_sup()
        self._sup.terminate_child(name)

    @keyword("T Sup Expect Child State")
    def t_sup_expect_child_state(
        self, name: str, state: str, within: str = "5s"
    ) -> None:
        self._require_sup()
        self._sup.expect_child_state(name, state, _seconds(within))

    @keyword("T Sup Expect Restart Count")
    def t_sup_expect_restart_count(
        self, name: str, count: int, within: str = "10s"
    ) -> None:
        self._require_sup()
        self._sup.expect_restart_count(name, int(count), _seconds(within))

    @keyword("T Sup Get Topology")
    def t_sup_get_topology(self) -> dict:
        self._require_sup()
        return self._sup.get_topology()

    # ──────────────────────────────────────────────────────────────────
    # T Sig — signal-flow tracing. Phase 1.3 / 1.4 fleshes these out.
    # ──────────────────────────────────────────────────────────────────

    @keyword("T Sig Open Trace")
    def t_sig_open_trace(self, source: str) -> None:
        from .adapters.tracer_jsonl import TraceFeed
        self._trace = TraceFeed(source)
        self._trace.open()
        logger.info("T Sig: trace feed open at %s", source)

    @keyword("T Sig Close Trace")
    def t_sig_close_trace(self) -> None:
        if self._trace is not None:
            self._trace.close()
            self._trace = None

    @keyword("T Sig Expect Trace")
    def t_sig_expect_trace(
        self, event: str, node: str = "", within: str = "2s"
    ) -> None:
        self._require_trace()
        self._trace.expect(event=event, node=node, timeout=_seconds(within))

    @keyword("T Sig Expect Order")
    def t_sig_expect_order(self, *events: str, same_correlation: bool = True) -> None:
        """Assert a sequence of trace events appears in order.

        Pass event names as positional args:

            T Sig Expect Order    send    recv    dispatch    dispatch_done

        ``same_correlation=True`` (default) requires the matched events
        to share a correlation_id — use this for RPC-style assertions.
        """
        self._require_trace()
        self._trace.expect_order(
            list(events), same_correlation=bool(same_correlation)
        )

    @keyword("T Sig Expect Latency")
    def t_sig_expect_latency(
        self, from_event: str, to_event: str, lt: str = "50ms"
    ) -> None:
        self._require_trace()
        self._trace.expect_latency(from_event, to_event, lt=_seconds(lt))

    @keyword("T Sig Filter Records")
    def t_sig_filter_records(self, **where: Any) -> Any:
        self._require_trace()
        return self._trace.filter(**where)

    # ──────────────────────────────────────────────────────────────────
    # TPT engine passthroughs — keep the rf-tpt-ls keyword names intact
    # so scenarios using TPT idioms look identical to the ancestor.
    # ──────────────────────────────────────────────────────────────────

    @keyword("Create Partition")
    def create_partition(self, name: str) -> None:
        self.engine.add_partition(Partition(name))

    @keyword("Add Transition")
    def add_transition(self, source: str, target: str, condition: str) -> None:
        guard = self._parse_condition(condition)
        self.engine.add_transition(source, target, guard, condition)

    @keyword("Set Signal")
    def set_signal(self, name: str, value: float) -> None:
        self.signals.set(name, float(value))

    @keyword("Apply Ramp")
    def apply_ramp(
        self, signal_name: str, start: float, end: float, duration: str
    ) -> None:
        self.engine.add_stimulus(
            RampStimulus(
                self.signals, signal_name,
                float(start), float(end), _seconds(duration),
            )
        )

    @keyword("Run Time Engine")
    def run_time_engine(self, initial: str = "", timeout: str = "60s") -> None:
        self.engine.timeout = _seconds(timeout)
        start = initial or next(iter(self.engine.partitions))
        self.engine.run(start)

    # ──────────────────────────────────────────────────────────────────
    # Utilities.
    # ──────────────────────────────────────────────────────────────────

    @keyword("T Wait")
    def t_wait(self, duration: str) -> None:
        import time
        time.sleep(_seconds(duration))

    # ──────────────────────────────────────────────────────────────────
    # Private helpers.
    # ──────────────────────────────────────────────────────────────────

    def _parse_condition(self, condition: str):
        """Parse a transition condition string into a guard.

        Mirrors rf-tpt-ls's _parse_condition. Accepts ``after Xs``,
        ``event:NAME``, and binary signal predicates (``Speed > 90``).
        """
        cond = condition.strip()
        if cond.startswith("after "):
            return time_guard(_seconds(cond[6:]))
        if cond.startswith("event:"):
            return event_guard(self.events, cond[6:].strip())
        for op in (">=", "<=", "!=", ">", "<", "=="):
            if op in cond:
                lhs, rhs = cond.split(op, 1)
                return signal_guard(
                    self.signals, lhs.strip(), op, float(rhs.strip())
                )
        raise ValueError(f"Cannot parse condition: {condition!r}")

    def _require_sup(self) -> None:
        if self._sup is None:
            raise RuntimeError(
                "supervisor not connected — call `T Sup Connect` first"
            )

    def _require_trace(self) -> None:
        if self._trace is None:
            raise RuntimeError(
                "trace feed not open — call `T Sig Open Trace` first"
            )


def _seconds(spec: str | float | int) -> float:
    """Parse a Robot-style duration. Accepts ``5s``, ``500ms``, plain
    numbers (seconds), or anything `float()` can swallow."""
    if isinstance(spec, (int, float)):
        return float(spec)
    s = str(spec).strip().lower()
    if s.endswith("ms"):
        return float(s[:-2]) / 1000.0
    if s.endswith("s"):
        return float(s[:-1])
    return float(s)

"""TheiaTestLibrary — Robot keyword surface for rf-theia.

The library is **routing only**. All semantics live in
:mod:`rf_theia.runtime`. Each keyword is a small method that forwards
to a runtime call; this keeps the library readable as a catalogue and
lets the runtime evolve without touching keyword definitions.

Phase-1 keywords (``T Sup *``, ``T Sig *``) remain importable for
backward compatibility but are no longer auto-exposed as Robot
keywords — they're plain methods now, used internally by adapters
that the role-named keywords drive.

Keyword catalogue (Pair 1 — hybrid automata):

  - ``Load Rig``           — bind a typed Rig context to the suite
  - ``Tear Down Rig``      — close adapters
  - ``Start State Machine``— instantiate a flow from the registry
  - ``Stop State Machine`` — stop a running flow
  - ``Emit Event``         — publish onto the runtime event bus
  - ``Wait For State``     — reactive block on FSM state entry
  - ``Verdict``            — TTCN-3-style outcome
"""
from __future__ import annotations

import logging
import time
from typing import Any, Optional

from robot.api.deco import keyword, library

# Vendored TPT engine (Phase 1 keywords still kept — useful for direct
# TPT scenarios that don't need the full flow engine).
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

# Pair-1 runtime — typed Rig, event bus, flow engine, supervisor watcher.
from .runtime import load_rig
from .runtime.event_bus import EventBus
from .runtime.flow_engine import FlowEngine, RuntimeContext
from .runtime.flows import RestartChild
from .runtime.supervisor_watcher import SupervisorWatcher

logger = logging.getLogger("rf_theia")


# Registry of available flows. New flows added here become callable
# via `Start State Machine <Name>` without touching keyword wiring.
FLOW_REGISTRY: dict[str, type] = {
    "RestartChild": RestartChild,
}


@library(scope="SUITE")
class TheiaTestLibrary:
    """Single-library entry point for rf-theia scenarios.

    Robot scenarios import this with::

        Library    rf_theia.TheiaTestLibrary
    """

    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self) -> None:
        # TPT engine — vendored, domain-agnostic. Kept for direct
        # TPT scenarios.
        self.signals = SignalStore()
        self.events = EventStore()
        self.engine = TimeEngine()

        # Pair-1 runtime — instantiated lazily by Load Rig.
        self._ctx: Optional[RuntimeContext] = None
        self._flow_engine: Optional[FlowEngine] = None
        self._sup_watcher: Optional[SupervisorWatcher] = None
        self._verdict: str = "none"

    # ──────────────────────────────────────────────────────────────────
    # Pair-1 lifecycle: Load Rig / Tear Down Rig.
    # ──────────────────────────────────────────────────────────────────

    @keyword("Load Rig")
    def load_rig_kw(
        self,
        path: str,
        supervisor: str = "localhost:5051",
    ) -> None:
        """Bind a typed Rig context from artheia's rig-deps JSON.

        Args:
          path:       Path to the rig.json (artheia rig-deps --out).
          supervisor: gRPC endpoint of services/com SupervisorView.

        Opens the supervisor channel lazily — connection failures
        surface on first use, not here, so a hermetic test can load
        a rig fixture without a live supervisor.
        """
        rig = load_rig(path)
        bus = EventBus()
        ctx = RuntimeContext(bus=bus, rig=rig, supervisor=None)

        # Lazy supervisor connect: try, but don't fail the load if no
        # supervisor is reachable. Flows that need it will fail their
        # entry action with a clear error.
        try:
            from .adapters.supervisor_grpc import SupervisorClient
            sup = SupervisorClient(supervisor)
            sup.connect(timeout=2.0)
            ctx.supervisor = sup
            watcher = SupervisorWatcher(bus, sup)
            watcher.start()
            self._sup_watcher = watcher
            logger.info("Load Rig: supervisor at %s connected", supervisor)
        except (AssertionError, Exception) as e:
            logger.warning(
                "Load Rig: supervisor at %s unreachable (%s) — "
                "flows requiring supervisor will fail at entry",
                supervisor, e,
            )

        self._ctx = ctx
        self._flow_engine = FlowEngine(ctx)
        self._verdict = "none"
        logger.info("Load Rig: %s (vehicle=%s, machines=%d, components=%d)",
                    path, rig.vehicle.name, len(rig.machines),
                    len(rig.all_components()))

    @keyword("Tear Down Rig")
    def tear_down_rig(self) -> None:
        """Stop flows, close adapters, drop the runtime context."""
        if self._flow_engine is not None:
            self._flow_engine.stop_all()
            self._flow_engine = None
        if self._sup_watcher is not None:
            self._sup_watcher.stop()
            self._sup_watcher = None
        if self._ctx is not None and self._ctx.supervisor is not None:
            try:
                self._ctx.supervisor.close()
            except Exception:
                pass
        self._ctx = None

    # ──────────────────────────────────────────────────────────────────
    # Pair-1 keywords: hybrid automata surface.
    # ──────────────────────────────────────────────────────────────────

    @keyword("Start State Machine")
    def start_state_machine(self, flow_name: str, **params: Any) -> None:
        """Instantiate and run a flow from the registry.

        Example::

            Start State Machine    RestartChild    target=sm_daemon

        The flow runs on a background thread. Use ``Wait For State``
        to react to its state transitions and ``Stop State Machine``
        to terminate it explicitly. Final states (e.g. ``Restarted``,
        ``Failure``) stop the flow naturally.
        """
        self._require_runtime()
        flow_cls = FLOW_REGISTRY.get(flow_name)
        if flow_cls is None:
            raise KeyError(
                f"Start State Machine: {flow_name!r} not in registry "
                f"(have: {sorted(FLOW_REGISTRY)})"
            )
        assert self._flow_engine is not None
        self._flow_engine.start(flow_cls, **params)
        logger.info("Start State Machine: %s started with params=%r",
                    flow_name, params)

    @keyword("Stop State Machine")
    def stop_state_machine(self, flow_name: str) -> None:
        self._require_runtime()
        assert self._flow_engine is not None
        self._flow_engine.stop(flow_name)

    @keyword("Emit Event")
    def emit_event(self, event_name: str, **payload: Any) -> None:
        """Publish an event onto the runtime bus.

        Flows whose current state has an ``event_name``-keyed
        transition will react. Payload keys flow through verbatim; flow
        guards can match on them (e.g. ``ev.payload.get('on') == target``).
        """
        self._require_runtime()
        assert self._ctx is not None
        self._ctx.bus.publish(event_name, **payload)

    @keyword("Wait For State")
    def wait_for_state(
        self,
        state_name: str,
        within: str = "5s",
        flow: str = "",
    ) -> None:
        """Block until a flow enters ``state_name``, or fail.

        With ``flow=""``, matches the first flow whose state machine
        enters ``state_name``. Set ``flow=<name>`` to scope the match.
        """
        self._require_runtime()
        assert self._ctx is not None
        target_flow = flow or None
        timeout = _seconds(within)

        def _match(ev) -> bool:
            if ev.payload.get("name") != state_name:
                return False
            if target_flow is not None and ev.payload.get("flow") != target_flow:
                return False
            return True

        # Anchor at the moment we begin waiting — earlier state entries
        # for previous tests shouldn't satisfy a fresh `Wait For State`.
        ev = self._ctx.bus.wait_for(
            "state_entered", match=_match, timeout=timeout,
            since=time.monotonic(),
        )
        if ev is None:
            raise AssertionError(
                f"Wait For State: did not see {state_name!r}"
                + (f" in flow {target_flow!r}" if target_flow else "")
                + f" within {within}"
            )
        logger.info("Wait For State: %s reached (flow=%s)",
                    state_name, ev.payload.get("flow"))

    @keyword("Verdict")
    def verdict(self, outcome: str) -> None:
        """Record the test verdict. TTCN-3 ordering applies:
        ``error > fail > inconclusive > pass > none``.

        Multiple calls within a suite merge: the worst verdict wins.
        Robot's own pass/fail still applies — Verdict is the framework's
        own outcome record, useful for cross-test aggregation in MCP /
        pandas analysis later.
        """
        order = {"none": 0, "pass": 1, "inconclusive": 2,
                 "fail": 3, "error": 4}
        new = outcome.strip().lower()
        if new not in order:
            raise ValueError(
                f"Verdict: {outcome!r} not in {sorted(order)}"
            )
        if order[new] > order.get(self._verdict, 0):
            self._verdict = new
        logger.info("Verdict: %s (suite verdict now %s)", new, self._verdict)

    # ──────────────────────────────────────────────────────────────────
    # TPT engine passthroughs — kept from Phase 1; declarative-shape.
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
        time.sleep(_seconds(duration))

    # ──────────────────────────────────────────────────────────────────
    # Private helpers.
    # ──────────────────────────────────────────────────────────────────

    def _parse_condition(self, condition: str):
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

    def _require_runtime(self) -> None:
        if self._ctx is None:
            raise RuntimeError(
                "rf-theia runtime not loaded — call `Load Rig` first"
            )


def _seconds(spec: str | float | int) -> float:
    if isinstance(spec, (int, float)):
        return float(spec)
    s = str(spec).strip().lower()
    if s.endswith("ms"):
        return float(s[:-2]) / 1000.0
    if s.endswith("s"):
        return float(s[:-1])
    return float(s)

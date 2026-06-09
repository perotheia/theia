"""Demo FSM statem-testing keywords — drive + observe a gen_statem FC.

Tests the demo DemoFsm gen_statem FC standalone:

  - INJECT events at the gate via artheia.probe (ProbeAdapter, ordered casts
    over one connection) — DemoFsm takes no wire messages; DemoFsmGate (tipc
    0xd0010007) receives DemoFsmIn events and post_event()s them into the FSM.
  - OBSERVE the resulting STATEM trace via artheia.observer (StatemObserver,
    TIPC firehose) — each committed transition publishes a `statem_transition`
    event on the bus carrying from/to state + the decoded FSM `data` (OTP
    `{State, Data}` Data term).

Role-named keywords (the v3 DSL surface, scoped to this scenario for now):
  Start Demo Fsm Stack / Stop Demo Fsm Stack — stage + run central, enable
                                               STATEM trace, attach observer.
  Emit Fsm Event   <Event>                   — probe-cast at the gate.
  Wait For Fsm State  <State>  within=2s     — reactive block on a transition.
  Assert Fsm Data  visits=N  reason=...      — assert the last data snapshot.

Path: rf test → ProbeAdapter → TIPC cast → DemoFsmGate → post_event →
DemoFsm FSM → STATEM trace → StatemObserver → bus. No com/gRPC: the observer
and the probe are both TIPC-direct (clients via .art + probe).

Prereq: binaries built + staged (demo/stage_local.sh). The suite stages + runs
the supervisor; it does NOT build. Tag 'live'.
"""
from __future__ import annotations

import os
import re
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

from robot.api.deco import keyword, library

# scenarios/demo/fsm/ → repo root is 5 parents up:
# fsm[0] demo[1] scenarios[2] rf_theia[3] testing[4] theia[5].
_WS = Path(__file__).resolve().parents[5]
for p in (str(_WS / "testing"), str(_WS / "artheia"),
          str(_WS / "tools" / "tdb")):
    if p not in sys.path:
        sys.path.insert(0, p)

from rf_theia.runtime.event_bus import EventBus           # noqa: E402
from rf_theia.runtime.statem_observer import StatemObserver  # noqa: E402
from rf_theia.runtime.probe_adapter import ProbeAdapter   # noqa: E402

CENTRAL_DIR = _WS / "install" / "central"
# DemoFsm + DemoFsmGate + DemoFsmTester all live in the demo package; load it
# through the canonical system/ path so cross-package imports resolve.
DEMO_ART = _WS / "system" / "demo" / "package.art"

FSM_NODE = "demo_fsm"          # the statem node's kNodeName (trace src)
GATE_NODE = "DemoFsmGate"      # the receiver node events are cast at
TESTER_NODE = "DemoFsmTester"  # the sender node the probe binds as source
TK_STATEM = 5


def _seconds(spec) -> float:
    s = str(spec).strip().lower()
    if s.endswith("ms"):
        return float(s[:-2]) / 1000.0
    if s.endswith("s"):
        return float(s[:-1])
    return float(s)


@library(scope="SUITE")
class DemoFsmLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self) -> None:
        self._sup: "subprocess.Popen | None" = None
        self._sup_log: "Path | None" = None
        self._bus = EventBus()
        self._obs: "StatemObserver | None" = None
        self._probe: "ProbeAdapter | None" = None
        self._wait_anchor = 0.0

    # ----- lifecycle --------------------------------------------------

    @keyword("Start Demo Fsm Stack")
    def start_stack(self, timeout: float = 10.0) -> None:
        """Stage install/central, launch the supervisor (THEIA_TRACE=1 so the
        FSM's tracer emits), enable STATEM trace on demo_fsm, then attach the
        observer + probe. After this, the FSM is at IDLE and observable."""
        try:
            socket.socket(socket.AF_TIPC, socket.SOCK_SEQPACKET).close()
        except OSError as e:
            raise AssertionError(f"AF_TIPC unavailable ({e}); modprobe tipc")

        env = os.environ.copy()
        env.pop("PYTHONPATH", None)
        r = subprocess.run(["bash", "demo/stage_local.sh"], cwd=str(_WS),
                           env=env, capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError(
                f"stage_local.sh failed:\n{r.stdout}\n{r.stderr}")

        self._sup_log = Path(f"/tmp/rf_demofsm_sup_{os.getpid()}.log")
        senv = dict(env, THEIA_TRACE="1", THEIA_LOG_LEVEL="info")
        self._sup = subprocess.Popen(
            ["./supervisor"],
            cwd=str(CENTRAL_DIR),
            stdout=open(self._sup_log, "w"), stderr=subprocess.STDOUT,
            env=dict(senv,
                     THEIA_SUPERVISOR_MANIFEST="executor.json",
                     THEIA_ROOT_DIR=".",
                     THEIA_SUPERVISOR_INSTANCE="0"),
            preexec_fn=os.setsid)
        # Wait for the FSM to bind (its first on_enter → IDLE).
        self._wait_log(self._sup_log, "[demo_fsm] → IDLE", timeout,
                       "demo_fsm to reach IDLE")

        # Enable STATEM trace on demo_fsm (supervisor pushes the config).
        self._enable_statem_trace(timeout=4.0)

        # Attach the observer (STATEM firehose → bus) + the probe (gate caster).
        self._obs = StatemObserver(self._bus, node=FSM_NODE).start(timeout=4.0)
        self._probe = ProbeAdapter(
            art=DEMO_ART, tester=TESTER_NODE, gate=GATE_NODE).start()
        # Give the observer a beat to settle its subscribe before events fly.
        time.sleep(0.5)

    @keyword("Stop Demo Fsm Stack")
    def stop_stack(self) -> None:
        if self._probe is not None:
            self._probe.stop(); self._probe = None
        if self._obs is not None:
            self._obs.stop(); self._obs = None
        if self._sup is not None and self._sup.poll() is None:
            try:
                os.killpg(os.getpgid(self._sup.pid), signal.SIGTERM)
                self._sup.wait(timeout=6)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                try:
                    os.killpg(os.getpgid(self._sup.pid), signal.SIGKILL)
                except ProcessLookupError:
                    pass
        self._sup = None

    # ----- drive ------------------------------------------------------

    @keyword("Emit Fsm Event")
    def emit_fsm_event(self, event: str, **fields) -> None:
        """Cast a DemoFsmIn event (DemoStart|DemoFinish|DemoReset) at the gate.

        Anchors the wait-window so a following `Wait For Fsm State` only
        matches transitions caused by THIS event, not stale ones."""
        if self._probe is None:
            raise AssertionError("Emit Fsm Event before Start Demo Fsm Stack")
        self._wait_anchor = time.monotonic()
        self._probe.emit(event, **fields)

    # ----- observe / assert -------------------------------------------

    @keyword("Wait For Fsm State")
    def wait_for_fsm_state(self, state: str, within: str = "2s") -> dict:
        """Block until demo_fsm enters ``state`` (reactive, via the STATEM
        trace), or fail. Returns the transition's payload (incl. the decoded
        ``data``) so a scenario can chain `Assert Fsm Data`."""
        timeout = _seconds(within)

        def _match(ev) -> bool:
            return ev.payload.get("to_state") == state

        ev = self._bus.wait_for(
            "statem_transition", match=_match, timeout=timeout,
            since=self._wait_anchor or None)
        if ev is None:
            raise AssertionError(
                f"demo_fsm did not enter {state!r} within {within} "
                f"(see {self._sup_log})")
        self._last = ev.payload
        return ev.payload

    @keyword("Assert Fsm Data")
    def assert_fsm_data(self, **expected) -> None:
        """Assert fields of the LAST observed transition's FSM data (OTP Data
        term). e.g. `Assert Fsm Data  visits=2  reason=PROCESSING`. Values are
        compared as strings (Robot args are strings)."""
        last = getattr(self, "_last", None)
        if last is None:
            raise AssertionError(
                "Assert Fsm Data before any Wait For Fsm State")
        data = last.get("data") or {}
        for key, want in expected.items():
            got = data.get(key)
            if str(got) != str(want):
                raise AssertionError(
                    f"FSM data[{key!r}]={got!r}, expected {want!r} "
                    f"(full data: {data!r})")

    # ----- internals --------------------------------------------------

    def _enable_statem_trace(self, timeout: float) -> None:
        """Push ConfigureTrace(demo_fsm, STATEM) via the tdb supervisor client,
        then confirm the supervisor logged the enable."""
        from tdb_client import SupervisorClient
        sup = SupervisorClient.from_workspace(_WS)
        try:
            sup.configure_trace(target_node=FSM_NODE, enabled=True,
                                kind=TK_STATEM)
        finally:
            try:
                sup.probe.stop()
            except Exception:
                pass
        line = self._grep(
            self._sup_log,
            re.compile(rf"trace config ENABLE kind {TK_STATEM} for {FSM_NODE}"),
            timeout)
        if line is None:
            raise AssertionError(
                f"supervisor never logged STATEM trace enable for {FSM_NODE} "
                f"(see {self._sup_log})")

    def _wait_log(self, path: Path, needle: str, timeout: float,
                  what: str) -> None:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if path.exists() and needle in path.read_text():
                return
            if self._sup and self._sup.poll() is not None:
                raise AssertionError(
                    f"supervisor exited before {what}:\n{path.read_text()}")
            time.sleep(0.1)
        raise AssertionError(
            f"timed out waiting for {what}:\n"
            f"{path.read_text() if path.exists() else '(no log)'}")

    def _grep(self, path: "Path | None", pat: "re.Pattern", timeout: float):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if path and path.exists():
                m = pat.search(path.read_text())
                if m:
                    return m.group(0)
            time.sleep(0.1)
        return None

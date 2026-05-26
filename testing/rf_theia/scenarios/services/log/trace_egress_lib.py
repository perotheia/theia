"""Live trace-egress e2e: node → collector → rf over the collector's gRPC.

Proves the egress-direct design end to end (docs/tasks/BACKLOG/
trace-to-rf-via-com.md):

  sm (reporting FC) --TIPC SOCK_DGRAM TraceRecord--> services/log[trace]
     collector (in_records 0x80010013) --gRPC TraceStream.Subscribe--> rf

com is NOT in the trace byte path. The collector rewrites src/dst from
TIPC addresses to component names via the cluster netgraph.json it
digests at startup.

Two checks:
  T1  egress under THEIA_TRACE=1: the supervisor boots sm with tracing
      on (boot switch), sm emits on every dispatch, and a gRPC subscriber
      receives sm's records — proving the producer + collector + gRPC
      hook.
  T2  control path: rf turns trace ON for a node via com's ConfigureTrace
      (rf → com → supervisor → node, op_kind=9) — proving the kind-push
      reaches the node. (Verified by the supervisor's push log; the
      egress itself is already shown by T1.)

Prereqs (suite stages, does NOT build):
  - install/central staged by demo/stage_local.sh (supervisor +
    executor.json + bin/<child> + services-log + netgraph.json).
  - CMake services-com (started after the supervisor for the control RPC).
"""
from __future__ import annotations

import os
import pathlib
import re
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

from robot.api.deco import keyword, library

# scenarios/services/log/ → repo root: log[0] services[1] scenarios[2]
# rf_theia[3] testing[4] theia[5].
_WS = Path(__file__).resolve().parents[5]
_SUPDBG = _WS / "tools" / "supdbg"
for p in (str(_SUPDBG), str(_SUPDBG / "_gen")):
    if p not in sys.path:
        sys.path.insert(0, p)

CENTRAL_DIR = _WS / "install" / "central"
COLLECTOR = CENTRAL_DIR / "services-log"
COM_BRIDGE = _WS / "services" / "com" / "build" / "services-com"
COLLECTOR_ENDPOINT = os.environ.get("THEIA_COLLECTOR_ENDPOINT", "127.0.0.1:7710")
COM_ENDPOINT = os.environ.get("THEIA_COM_ENDPOINT", "localhost:7700")

# TraceKind ordinals (services/log + platform_runtime TraceKind — aligned).
TK_STATEM = 5


@library(scope="SUITE")
class TraceEgressLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self) -> None:
        self._sup: subprocess.Popen | None = None
        self._collector: subprocess.Popen | None = None
        self._com: subprocess.Popen | None = None
        self._sup_log: Path | None = None
        self._collector_log: Path | None = None
        self._com_log: Path | None = None

    # ----- staging + lifecycle ----------------------------------------

    @keyword("Stage And Start Central With Tracing")
    def stage_and_start_central(self) -> None:
        """Lay out install/central, then launch the supervisor with
        THEIA_TRACE=1 so every reporting node's tracer emits on boot."""
        if not COLLECTOR.exists():
            raise AssertionError(
                f"collector not staged at {COLLECTOR} — "
                f"cmake --build services/log/build && bash demo/stage_local.sh")
        try:
            socket.socket(socket.AF_TIPC, socket.SOCK_DGRAM).close()
        except OSError as e:
            raise AssertionError(f"AF_TIPC unavailable ({e}); modprobe tipc")

        env = os.environ.copy()
        env.pop("PYTHONPATH", None)
        r = subprocess.run(["bash", "demo/stage_local.sh"], cwd=str(_WS),
                           env=env, capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError(f"stage_local.sh failed:\n{r.stdout}\n{r.stderr}")

        self._sup_log = Path(f"/tmp/rf_trace_sup_{os.getpid()}.log")
        senv = dict(env, THEIA_TRACE="1", THEIA_LOG_LEVEL="debug")
        self._sup = subprocess.Popen(
            ["./supervisor", "run", "executor.json",
             "--root-dir", ".", "--machine-name", "central_host"],
            cwd=str(CENTRAL_DIR),
            stdout=open(self._sup_log, "w"), stderr=subprocess.STDOUT,
            env=senv, preexec_fn=os.setsid)
        self._wait_log(self._sup_log, "[sm_daemon] up", 8.0, "sm to bind")

    @keyword("Start Collector")
    def start_collector(self, timeout: float = 8.0) -> None:
        """Launch the services/log[trace] collector (its own gRPC on
        :7710) with the staged netgraph for addr->name rewrite."""
        self._collector_log = Path(f"/tmp/rf_trace_collector_{os.getpid()}.log")
        env = os.environ.copy(); env.pop("PYTHONPATH", None)
        self._collector = subprocess.Popen(
            ["./services-log", "--listen", COLLECTOR_ENDPOINT,
             "--netgraph", "netgraph.json"],
            cwd=str(CENTRAL_DIR),
            stdout=open(self._collector_log, "w"), stderr=subprocess.STDOUT,
            env=env, preexec_fn=os.setsid)
        self._wait_tcp(COLLECTOR_ENDPOINT, timeout, self._collector,
                       self._collector_log, "collector gRPC")

    @keyword("Start Com Bridge")
    def start_com_bridge(self, timeout: float = 8.0) -> None:
        """Launch the com gRPC bridge (after the supervisor — it connects
        to the supervisor's TIPC at startup). For the control path RPC."""
        if not COM_BRIDGE.exists():
            raise AssertionError(f"com bridge not built at {COM_BRIDGE}")
        self._com_log = Path(f"/tmp/rf_trace_com_{os.getpid()}.log")
        env = os.environ.copy(); env.pop("PYTHONPATH", None)
        self._com = subprocess.Popen(
            [str(COM_BRIDGE), "--listen", "0.0.0.0:7700"],
            stdout=open(self._com_log, "w"), stderr=subprocess.STDOUT,
            env=env, preexec_fn=os.setsid)
        self._wait_tcp("localhost:7700", timeout, self._com, self._com_log,
                       "com gRPC")

    # ----- T1: egress (node → collector → rf gRPC) --------------------

    @keyword("Subscribe And Collect Sm Trace")
    def subscribe_and_collect(self, want: int = 1, kind: int = 0,
                              within: float = 6.0) -> int:
        """Open the collector's gRPC TraceStream and collect records whose
        src is the sm node, until `want` arrive or `within` elapses. The
        supervisor drives sm to RUNNING at boot (T1 of the gate work), so
        sm emits state_transition records with THEIA_TRACE=1. Returns the
        count of sm records seen; raises if zero."""
        from client import Client

        got: list = []
        stop = threading.Event()

        def run():
            c = Client(COM_ENDPOINT, collector_endpoint=COLLECTOR_ENDPOINT)
            try:
                for rec, _ in c.subscribe_traces(kind=kind):
                    # src node_name is rewritten to the component name; the
                    # sm statem node's kNodeName is "sm_daemon".
                    if "sm" in rec.node_name.lower():
                        got.append(rec)
                        if len(got) >= want:
                            break
                    if stop.is_set():
                        break
            except Exception as e:  # stream closed on teardown
                if not stop.is_set():
                    print(f"subscribe error: {e}")
            finally:
                c.close()

        t = threading.Thread(target=run, daemon=True)
        t.start()
        t.join(timeout=within)
        stop.set()

        if not got:
            log = (self._collector_log.read_text()
                   if self._collector_log and self._collector_log.exists()
                   else "(no collector log)")
            raise AssertionError(
                f"no sm trace records arrived on the collector gRPC within "
                f"{within}s. Collector log:\n{log[-1500:]}")
        return len(got)

    @keyword("Collector Saw Subscriber")
    def collector_saw_subscriber(self, timeout: float = 3.0) -> None:
        """Assert the collector logged a gRPC subscriber attach — proves
        the rf→collector gRPC leg connected."""
        line = self._grep(self._collector_log,
                          re.compile(r"gRPC subscriber attached"), timeout)
        if line is None:
            raise AssertionError("collector never logged a subscriber attach")

    # ----- T2: control path (rf → com → supervisor → node) ------------

    @keyword("Configure Trace For Sm Via Com")
    def configure_trace_via_com(self, kind: int = TK_STATEM) -> None:
        """rf → com.ConfigureTrace(node=sm_daemon, kind) → supervisor →
        TraceControlPush to the node. Asserts the supervisor logged the
        push (the node-side enable is observable as the trace flowing)."""
        from client import Client
        c = Client(COM_ENDPOINT)
        try:
            reply = c.configure_trace("sm_daemon", "", enabled=True)
        finally:
            c.close()
        # ConfigureTrace returns a ControlReply; status 0 = accepted.
        if getattr(reply, "status", 0) != 0:
            raise AssertionError(
                f"ConfigureTrace refused: status={reply.status} "
                f"msg={getattr(reply, 'message', '')}")

    @keyword("Supervisor Pushed Trace Config")
    def supervisor_pushed_trace(self, timeout: float = 4.0) -> None:
        line = self._grep(self._sup_log,
                          re.compile(r"pushed \d+ trace config|trace config "
                                     r"(ENABLE|DISABLE)"), timeout)
        if line is None:
            raise AssertionError(
                f"supervisor never logged a trace-config push; "
                f"see {self._sup_log}")

    # ----- teardown ---------------------------------------------------

    @keyword("Stop Trace Egress Stack")
    def stop_stack(self) -> None:
        for proc in (self._com, self._collector, self._sup):
            if proc is not None and proc.poll() is None:
                try:
                    os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
                    proc.wait(timeout=6)
                except (ProcessLookupError, subprocess.TimeoutExpired):
                    try:
                        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                    except ProcessLookupError:
                        pass
        self._com = self._collector = self._sup = None

    # ----- internals --------------------------------------------------

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
        raise AssertionError(f"timed out waiting for {what}:\n"
                             f"{path.read_text() if path.exists() else '(no log)'}")

    def _wait_tcp(self, endpoint: str, timeout: float, proc, log: Path,
                  what: str) -> None:
        host, _, port = endpoint.rpartition(":")
        host = host or "127.0.0.1"
        deadline = time.time() + timeout
        while time.time() < deadline:
            s = socket.socket(); s.settimeout(0.3)
            try:
                s.connect((host, int(port))); s.close(); return
            except OSError:
                s.close()
            if proc and proc.poll() is not None:
                raise AssertionError(
                    f"{what} exited early; see {log}:\n"
                    f"{log.read_text() if log.exists() else ''}")
            time.sleep(0.1)
        raise AssertionError(f"{what} not listening on {endpoint} within {timeout}s")

    def _grep(self, path: Path, pat: re.Pattern, timeout: float):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if path and path.exists():
                m = pat.search(path.read_text())
                if m:
                    return m.group(0)
            time.sleep(0.1)
        return None

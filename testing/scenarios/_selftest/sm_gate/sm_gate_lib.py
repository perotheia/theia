"""SM gate + probe re-send selftest (T1 + T2).

Stands up the central supervisor (which on boot casts SystemBoot +
StartupComplete to sm's GATE node — T1, driving the FSM OFF→…→RUNNING),
plus the services/com gRPC bridge. Then a test PROBE re-sends
StartupComplete to the same gate over the bridge, impersonating the
supervisor (T2) — sm is already RUNNING, so the gate forwards it and the
statem receives-and-ignores it (no transition out of RUNNING).

Architecture under test:
  test → com gRPC InjectSignal → robot probe → TIPC cast → SmGate
       → post_event() in-process → SmDaemon FSM.

The gate (TIPC 0x8001001D) is the FC's only TIPC-reachable surface for
FSM events; the statem node (0x8001000D) never takes wire messages
directly. The probe carries com's distinct identity.

Prereqs (the suite does NOT build):
  - bazel-built FC daemons + demo apps + the CMake supervisor, staged by
    apps/stage_local.sh into install/central/.
  - the CMake gRPC bridge services/com/build/services-com (needs the
    supervisor's TIPC up first, so it's started AFTER the supervisor).

Only used by sm_gate_selftest.robot.
"""
from __future__ import annotations

import os
import pathlib
import re
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

from robot.api.deco import keyword, library

# scenarios/_selftest/sm_gate/ → repo root is 5 parents up.
_WS = Path(__file__).resolve().parents[4]
_SUPDBG = _WS / "tools" / "supdbg" / "_gen"
for p in (str(_SUPDBG), str(_SUPDBG / "fc")):
    if p not in sys.path:
        sys.path.insert(0, p)

CENTRAL_DIR = _WS / "install" / "central"
COM_BRIDGE = _WS / "bazel-bin" / "services" / "com" / "main" / "com"

# SmGate's TIPC name (services/system/sm/package.art) — the gate, NOT the
# statem node. Lifecycle events arrive here and post_event into the FSM.
SM_GATE_TIPC_TYPE = 0x8001001D
SM_GATE_TIPC_INSTANCE = 0
STARTUP_COMPLETE_TYPE = "services_services_sm_StartupComplete"
COM_ENDPOINT = os.environ.get("THEIA_COM_ENDPOINT", "localhost:7700")


@library(scope="SUITE")
class SmGateLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self) -> None:
        self._sup: subprocess.Popen | None = None
        self._com: subprocess.Popen | None = None
        self._sup_log: Path | None = None
        self._com_log: Path | None = None

    # ----- staging + lifecycle ----------------------------------------

    @keyword("Stage And Start Central")
    def stage_and_start_central(self) -> None:
        """Lay out install/central via apps/stage_local.sh, then launch
        the supervisor (THEIA_TRACE=1 so sm's tracer emits). The
        supervisor's T1 handshake drives sm to RUNNING."""
        if not COM_BRIDGE.exists():
            raise AssertionError(
                f"com binary not built at {COM_BRIDGE} — "
                f"bazel build //services/com/main:com")
        try:
            socket.socket(socket.AF_TIPC, socket.SOCK_SEQPACKET).close()
        except OSError as e:
            raise AssertionError(f"AF_TIPC unavailable ({e}); modprobe tipc")

        env = os.environ.copy()
        env.pop("PYTHONPATH", None)
        r = subprocess.run(["bash", "apps/stage_local.sh"], cwd=str(_WS),
                           env=env, capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError(f"stage_local.sh failed:\n{r.stdout}\n{r.stderr}")

        self._sup_log = Path(f"/tmp/rf_smgate_sup_{os.getpid()}.log")
        senv = dict(env, THEIA_TRACE="1", THEIA_LOG_LEVEL="debug")
        self._sup = subprocess.Popen(
            ["./supervisor", "run", "executor.json",
             "--root-dir", ".", "--machine-name", "central_host"],
            cwd=str(CENTRAL_DIR),
            stdout=open(self._sup_log, "w"), stderr=subprocess.STDOUT,
            env=senv, preexec_fn=os.setsid)
        self._wait_log(self._sup_log, "[sm_gate] up", 8.0, "sm gate to bind")

    @keyword("Sm Reached Running")
    def sm_reached_running(self, timeout: float = 8.0) -> str:
        """T1: assert the supervisor's handshake drove sm to RUNNING."""
        line = self._grep(self._sup_log,
                          re.compile(r"sm_daemon\] → RUNNING"), timeout)
        if line is None:
            raise AssertionError(
                f"sm did not reach RUNNING within {timeout}s; "
                f"see {self._sup_log}")
        return line

    @keyword("Start Com Bridge")
    def start_com_bridge(self, timeout: float = 8.0) -> None:
        """Launch the gRPC bridge (after the supervisor — it connects to
        the supervisor's TIPC at startup) and wait for :7700."""
        self._com_log = Path(f"/tmp/rf_smgate_com_{os.getpid()}.log")
        env = os.environ.copy(); env.pop("PYTHONPATH", None)
        # Native com binary takes NO argv; gRPC listen addr from env.
        env["THEIA_COM_LISTEN"] = "0.0.0.0:7700"
        self._com = subprocess.Popen(
            [str(COM_BRIDGE)],
            stdout=open(self._com_log, "w"), stderr=subprocess.STDOUT,
            env=env, preexec_fn=os.setsid)
        deadline = time.time() + timeout
        while time.time() < deadline:
            s = socket.socket(); s.settimeout(0.3)
            try:
                s.connect(("localhost", 7700)); s.close(); return
            except OSError:
                s.close()
            if self._com.poll() is not None:
                raise AssertionError(
                    f"com bridge exited early; see {self._com_log}:\n"
                    f"{self._com_log.read_text()}")
            time.sleep(0.1)
        raise AssertionError(f"com bridge not listening on 7700 within {timeout}s")

    # ----- T2: probe re-send ------------------------------------------

    @keyword("Probe Resend Startup Complete")
    def probe_resend_startup_complete(self) -> bool:
        """T2: the probe re-sends StartupComplete to the gate over the
        com gRPC bridge, impersonating the supervisor. Returns the ack's
        `sent`."""
        import grpc
        import supervisor_bridge_pb2 as pb
        import supervisor_bridge_pb2_grpc as rpc
        from sm import sm_pb2

        stub = rpc.SupervisorViewStub(grpc.insecure_channel(COM_ENDPOINT))
        ack = stub.InjectSignal(pb.InjectSignalCall(
            tipc_type=SM_GATE_TIPC_TYPE,
            tipc_instance=SM_GATE_TIPC_INSTANCE,
            msg_type=STARTUP_COMPLETE_TYPE,
            payload=sm_pb2.StartupComplete().SerializeToString(),
            src="ProbeImpersonatingSupervisor"))
        return bool(ack.sent)

    @keyword("Gate Forwarded Count")
    def gate_forwarded_count(self, timeout: float = 4.0,
                             at_least: int = 3) -> int:
        """Count `[sm_gate] StartupComplete → post_event` lines. T1 sends
        one; the probe re-send adds another, so >=3 total StartupComplete
        forwards (1 from T1) + the resend... we assert at_least."""
        deadline = time.time() + timeout
        pat = re.compile(r"sm_gate\] StartupComplete → post_event")
        while time.time() < deadline:
            txt = self._sup_log.read_text() if self._sup_log.exists() else ""
            n = len(pat.findall(txt))
            if n >= at_least:
                return n
            time.sleep(0.1)
        txt = self._sup_log.read_text() if self._sup_log.exists() else ""
        return len(pat.findall(txt))

    @keyword("Sm Still Running")
    def sm_still_running(self) -> None:
        """T2: sm's LAST state transition must still be RUNNING — a
        repeated StartupComplete has no transition out of RUNNING, so the
        FSM stays put (received-and-ignored). We look at the sequence of
        `sm_daemon] → <STATE>` lines and assert the final one is RUNNING
        (→ OFF / → STARTING appear earlier, at boot, and are expected)."""
        txt = self._sup_log.read_text() if self._sup_log.exists() else ""
        states = re.findall(r"sm_daemon\] → (\w+)", txt)
        if not states:
            raise AssertionError(f"no sm state transitions in:\n{txt}")
        if states[-1] != "RUNNING":
            raise AssertionError(
                f"sm's last state is {states[-1]!r}, not RUNNING — it left "
                f"RUNNING after the probe re-send. Sequence: {states}")

    # ----- teardown ---------------------------------------------------

    @keyword("Stop Sm Gate Stack")
    def stop_stack(self) -> None:
        for proc in (self._com, self._sup):
            if proc is not None and proc.poll() is None:
                try:
                    os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
                    proc.wait(timeout=6)
                except (ProcessLookupError, subprocess.TimeoutExpired):
                    try:
                        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                    except ProcessLookupError:
                        pass
        self._com = self._sup = None

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

    def _grep(self, path: Path, pat: re.Pattern, timeout: float):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if path and path.exists():
                m = pat.search(path.read_text())
                if m:
                    return m.group(0)
            time.sleep(0.1)
        return None

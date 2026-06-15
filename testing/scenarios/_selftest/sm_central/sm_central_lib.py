"""Send a message to SM through a running CENTRAL supervisor stack.

Unlike the minimal sm-only e2e (robot_node/sm_signal_e2e), this starts
the whole central machine via its supervisor + executor.json — sm comes
up as a supervised child under core_sup, beside com/per/ucm and the
p1/p2 demo apps. We then impersonate a node and cast an SmRequest at
sm's TIPC address, and read the result back from sm's tracer (the
supervisor inherits THEIA_TRACE=1 and setenvs it onto every child, so
sm's TRC lines land in the supervisor's combined stdout).

Prereqs (staged by apps/stage_local.sh, built beforehand):
  install/central/supervisor      — CMake-built OTP supervisor
  install/central/executor.json   — CentralRig tree (sm under core_sup)
  install/central/bin/{sm,com,per,ucm,p1,p2}

This is the "start central, send a message to sm from robot" loop:
prove the inject reaches sm inside the live supervised stack and is
observable in its trace.
"""
from __future__ import annotations

import os
import pathlib
import re
import signal
import socket
import struct
import subprocess
import sys
import time

from robot.api.deco import keyword, library

# scenarios/_selftest/sm_central/ → repo root is 5 parents up.
_WS = pathlib.Path(__file__).resolve().parents[4]
_GEN_FC = _WS / "tools" / "supdbg" / "_gen" / "fc"
if str(_GEN_FC) not in sys.path:
    sys.path.insert(0, str(_GEN_FC))

CENTRAL_DIR = _WS / "install" / "central"
SM_TIPC_TYPE = 0x8001000D
SM_TIPC_INSTANCE = 0
SM_REQUEST_TYPE = "services_services_sm_SmRequest"


def _djb2_low16(s: str) -> int:
    h = 5381
    for c in s.encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h & 0xFFFF


@library(scope="SUITE")
class SmCentralLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self) -> None:
        self._proc: subprocess.Popen | None = None
        self._log_path: str | None = None

    # ----- lifecycle --------------------------------------------------

    @keyword("Start Central")
    def start_central(self) -> str:
        """Launch install/central/supervisor against its executor.json,
        cwd'd into the machine dir so `bin/<child>` resolves. The whole
        env is inherited by children, so THEIA_TRACE=1 turns on sm's
        tracer. Waits for sm's TIPC bind so the inject can't race
        startup. Returns the combined-log path."""
        sup = CENTRAL_DIR / "supervisor"
        exe = CENTRAL_DIR / "executor.json"
        if not sup.exists() or not exe.exists():
            raise AssertionError(
                f"central not staged at {CENTRAL_DIR} — run "
                f"`bash apps/stage_local.sh` (after building the binaries)")
        # AF_TIPC must be usable, else sm can't bind.
        try:
            socket.socket(socket.AF_TIPC, socket.SOCK_SEQPACKET).close()
        except OSError as e:
            raise AssertionError(f"AF_TIPC unavailable ({e}); modprobe tipc")

        self._log_path = f"/tmp/rf_sm_central_{os.getpid()}.log"
        env = dict(os.environ, THEIA_TRACE="1", THEIA_LOG_LEVEL="debug")
        log = open(self._log_path, "w")
        self._proc = subprocess.Popen(
            ["./supervisor", "run", "executor.json",
             "--root-dir", ".", "--machine-name", "central_host"],
            cwd=str(CENTRAL_DIR),
            stdout=log, stderr=subprocess.STDOUT, env=env,
            preexec_fn=os.setsid,   # own group → clean group kill
        )
        # sm starts first under core_sup; wait for its TIPC bind.
        self._wait_for("[sm_daemon] up", timeout=8.0)
        return self._log_path

    @keyword("Stop Central")
    def stop_central(self) -> None:
        if self._proc is None:
            return
        if self._proc.poll() is None:
            try:
                os.killpg(os.getpgid(self._proc.pid), signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                self._proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                os.killpg(os.getpgid(self._proc.pid), signal.SIGKILL)
                self._proc.wait(timeout=2)
        self._proc = None

    # ----- inject -----------------------------------------------------

    @keyword("Send Sm Request")
    def send_sm_request(self, target_state: str = "RUNNING") -> str:
        """Build SmRequest{target=<state>} host-side (FFI-free python
        protobuf encode) and cast it to sm over the robot-node wire
        shape (GW_MSG_GEN_CAST, service_id = djb2 of the nanopb C type
        name). Returns the payload hex for assertions."""
        from sm import sm_pb2
        state = getattr(sm_pb2, target_state.strip().upper())
        payload = sm_pb2.SmRequest(target=state).SerializeToString()
        svc = _djb2_low16(SM_REQUEST_TYPE)
        # 24-byte GwMessageHeader: bus=2 (RPC), kind=0x20 (GEN_CAST),
        # len, corr=0, service_id, ...
        hdr = struct.pack("<BBHQHHIH2x", 2, 0x20, len(payload), 0, svc, 0, 0, 0)
        s = socket.socket(socket.AF_TIPC, socket.SOCK_SEQPACKET)
        s.connect((socket.TIPC_ADDR_NAME, SM_TIPC_TYPE, SM_TIPC_INSTANCE,
                   SM_TIPC_INSTANCE, socket.TIPC_NODE_SCOPE))
        s.sendall(hdr + payload)
        s.close()
        return payload.hex()

    # ----- read back from sm's trace ----------------------------------

    @keyword("Sm Trace Should Show Recv")
    def sm_trace_should_show_recv(self, payload_hex: str = "",
                                  timeout: float = 5.0) -> str:
        pat = re.compile(
            rf"TRC v1 recv sm_daemon msg={re.escape(SM_REQUEST_TYPE)} "
            rf".*hex={payload_hex}")
        line = self._grep_log(pat, timeout)
        if line is None:
            raise AssertionError(
                f"no sm `recv` trace for {SM_REQUEST_TYPE} "
                f"(hex={payload_hex!r}) within {timeout}s; see {self._log_path}")
        return line

    @keyword("Sm Trace Should Show Dispatch")
    def sm_trace_should_show_dispatch(self, timeout: float = 5.0) -> str:
        pat = re.compile(
            rf"TRC v1 dispatch sm_daemon msg={re.escape(SM_REQUEST_TYPE)}")
        line = self._grep_log(pat, timeout)
        if line is None:
            raise AssertionError(
                f"no sm `dispatch` trace for {SM_REQUEST_TYPE} within "
                f"{timeout}s; see {self._log_path}")
        return line

    @keyword("Sm Log Should Contain")
    def sm_log_should_contain(self, needle: str, timeout: float = 5.0) -> str:
        line = self._grep_log(re.compile(re.escape(needle)), timeout)
        if line is None:
            raise AssertionError(
                f"sm log never contained {needle!r} within {timeout}s; "
                f"see {self._log_path}")
        return line

    # ----- internals --------------------------------------------------

    def _wait_for(self, needle: str, timeout: float) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self._log_path and os.path.exists(self._log_path):
                with open(self._log_path) as f:
                    if needle in f.read():
                        return
            if self._proc and self._proc.poll() is not None:
                raise AssertionError(
                    f"central supervisor exited early "
                    f"(rc={self._proc.returncode}); see {self._log_path}")
            time.sleep(0.05)
        raise AssertionError(
            f"central didn't log {needle!r} within {timeout}s; "
            f"see {self._log_path}")

    def _grep_log(self, pat: re.Pattern, timeout: float):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self._log_path and os.path.exists(self._log_path):
                with open(self._log_path) as f:
                    for ln in f:
                        if pat.search(ln):
                            return ln.rstrip()
            time.sleep(0.05)
        return None

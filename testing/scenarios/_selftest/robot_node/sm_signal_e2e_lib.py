"""First e2e: inject a signal at SM, read it back from the trace stream.

Stands up a standalone sm daemon (THEIA_TRACE=1 so its Tracer emits to
stderr without a supervisor), injects an SmRequest over the robot-node
wire shape, and reads the trace back.

Two trace observation points, tried in order:
  1. gRPC TraceStream.Subscribe via services/com — the "proper" path,
     but it needs the log[trace] collector running. SKIPPED here.
  2. sm's stderr trace (TRC v1 recv ... msg=...) — works in the minimal
     sm-only stack, which is what this first e2e exercises.

This is debug-grade scaffolding for the first signal e2e — it shells out
to the bazel-built sm binary and the AF_TIPC socket directly, no
supervisor / no com required.
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

_WS = pathlib.Path(__file__).resolve().parents[4]
_GEN_FC = _WS / "tools" / "supdbg" / "_gen" / "fc"
for p in (str(_GEN_FC),):
    if p not in sys.path:
        sys.path.insert(0, p)

SM_BIN = _WS / "bazel-bin" / "services" / "sm" / "main" / "sm"
SM_TIPC_TYPE = 0x8001000D
SM_TIPC_INSTANCE = 0
SM_REQUEST_TYPE = "services_services_sm_SmRequest"


def _djb2_low16(s: str) -> int:
    h = 5381
    for c in s.encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h & 0xFFFF


@library(scope="SUITE")
class SmSignalE2ELib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self) -> None:
        self._proc: subprocess.Popen | None = None
        self._log_path: str | None = None

    # ----- lifecycle --------------------------------------------------

    @keyword("Start Sm With Trace")
    def start_sm_with_trace(self) -> str:
        """Launch the bazel-built sm with THEIA_TRACE=1 so its tracer
        emits to stderr. Returns the log path. Skips the whole suite if
        the binary isn't built or AF_TIPC isn't available."""
        if not SM_BIN.exists():
            raise AssertionError(
                f"sm binary not built at {SM_BIN} — run "
                f"`bazel build //services/sm/main:sm`")
        # AF_TIPC must be usable, else the daemon can't bind.
        try:
            socket.socket(socket.AF_TIPC, socket.SOCK_SEQPACKET).close()
        except OSError as e:
            raise AssertionError(f"AF_TIPC unavailable ({e}); modprobe tipc")

        self._log_path = f"/tmp/rf_sm_e2e_{os.getpid()}.log"
        env = dict(os.environ, THEIA_TRACE="1", THEIA_LOG_LEVEL="debug")
        log = open(self._log_path, "w")
        self._proc = subprocess.Popen(
            [str(SM_BIN)], stdout=log, stderr=subprocess.STDOUT, env=env)
        # Wait for the TIPC bind line so the inject can't race startup.
        self._wait_for("bound {0x8001000D", timeout=5.0)
        return self._log_path

    @keyword("Stop Sm")
    def stop_sm(self) -> None:
        if self._proc is not None:
            self._proc.send_signal(signal.SIGTERM)
            try:
                self._proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self._proc.kill()
            self._proc = None

    # ----- inject -----------------------------------------------------

    @keyword("Inject Sm Request Cast")
    def inject_sm_request_cast(self, target_state: str = "RUNNING") -> str:
        """Build SmRequest{target=<state>} host-side and cast it to sm
        over the robot-node wire shape (GW_MSG_GEN_CAST). Returns the
        payload hex for assertions."""
        from sm import sm_pb2
        state = getattr(sm_pb2, target_state.strip().upper())
        payload = sm_pb2.SmRequest(target=state).SerializeToString()
        svc = _djb2_low16(SM_REQUEST_TYPE)
        # 24-byte GwMessageHeader: GEN_CAST=0x20, BUS_RPC=2, corr=0.
        hdr = struct.pack("<BBHQHHIH2x", 2, 0x20, len(payload), 0, svc, 0, 0, 0)
        s = socket.socket(socket.AF_TIPC, socket.SOCK_SEQPACKET)
        s.connect((socket.TIPC_ADDR_NAME, SM_TIPC_TYPE, SM_TIPC_INSTANCE,
                   SM_TIPC_INSTANCE, socket.TIPC_NODE_SCOPE))
        s.sendall(hdr + payload)
        s.close()
        return payload.hex()

    # ----- read back from trace ---------------------------------------

    @keyword("Trace Should Show Recv")
    def trace_should_show_recv(self, msg_type: str = SM_REQUEST_TYPE,
                               payload_hex: str = "",
                               timeout: float = 3.0) -> str:
        """Poll sm's stderr trace for a `recv` record of `msg_type`
        (optionally matching `payload_hex`). Returns the matched line."""
        pat = re.compile(
            rf"TRC v1 recv \S+ msg={re.escape(msg_type)} .*hex={payload_hex}")
        line = self._grep_log(pat, timeout)
        if line is None:
            raise AssertionError(
                f"no `recv` trace for {msg_type} (hex={payload_hex!r}) "
                f"within {timeout}s; see {self._log_path}")
        return line

    @keyword("Trace Should Show Dispatch")
    def trace_should_show_dispatch(self, msg_type: str = SM_REQUEST_TYPE,
                                   timeout: float = 3.0) -> str:
        pat = re.compile(rf"TRC v1 dispatch \S+ msg={re.escape(msg_type)}")
        line = self._grep_log(pat, timeout)
        if line is None:
            raise AssertionError(
                f"no `dispatch` trace for {msg_type} within {timeout}s")
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
                    f"sm exited early (rc={self._proc.returncode}); "
                    f"see {self._log_path}")
            time.sleep(0.05)
        raise AssertionError(f"sm didn't log {needle!r} within {timeout}s")

    def _grep_log(self, pat: re.Pattern, timeout: float):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with open(self._log_path) as f:
                for ln in f:
                    if pat.search(ln):
                        return ln.rstrip()
            time.sleep(0.05)
        return None

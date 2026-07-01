"""Crash-investigation trace persistence: set → read back → crash → re-apply.

The operator scenario: before (or during) a fault investigation you turn on
tracing for a module via the com gRPC bridge, confirm with the supervisor
what tracing is armed, then — when the child crashes and the supervisor
restarts it — the supervisor RE-APPLIES the stored trace config to the fresh
child without the operator touching anything. So the trace you armed
survives the crash you were trying to investigate.

Wire:
  set       rf --gRPC ConfigureTrace(sm_daemon, TK_STATEM)--> com
                 --TIPC ControlRequest op_kind=9--> supervisor
                    (stores trace_configs_["sm"], pushes TraceControlPush)
  read back rf --gRPC GetTraceConfig--> com
                 --TIPC ControlRequest op_kind=10--> supervisor
                    (replies TraceConfigList inline in ControlReply)
  crash     test SIGKILLs the sm child process
  re-apply  supervisor's restart strategy respawns sm; the first
                 heartbeat-after-gap re-fires push_trace_config_to_child("sm")
                 (#361) — observable as a SECOND "pushed N trace config
                 entries to 'sm'" in the supervisor log — and the read-back
                 still shows the entry.

Reuses the same staged install/central + collector + com bridge as
trace_egress (apps/stage_local.sh). com is the gRPC bridge; the supervisor
is the persistence authority.
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

# scenarios/services/log/ → repo root: log[0] services[1] scenarios[2]
# rf_theia[3] testing[4] theia[5].
_WS = Path(__file__).resolve().parents[4]
_SUPDBG = _WS / "tools" / "supdbg"
for p in (str(_SUPDBG), str(_SUPDBG / "_gen")):
    if p not in sys.path:
        sys.path.insert(0, p)

CENTRAL_DIR = _WS / "install" / "central"
COM_BRIDGE = _WS / "bazel-bin" / "services" / "com" / "main" / "com"

# The Bazel-built supervisor dynamically links libetcd-cpp-api.so (the
# etcd_publisher mirror). It's vendored, not on the default loader path, so the
# launcher MUST put it on LD_LIBRARY_PATH or the supervisor dies at exec with
# "libetcd-cpp-api.so: cannot open shared object file". Self-contained: don't
# rely on the caller's environment having set it.
_ETCD_LIB = _WS / "third_party" / "etcd-cpp-apiv3" / "install" / "lib"


def _stack_env() -> dict:
    """os.environ minus PYTHONPATH, with the vendored etcd lib dir prepended to
    LD_LIBRARY_PATH so the Bazel supervisor can resolve libetcd-cpp-api.so."""
    env = os.environ.copy()
    env.pop("PYTHONPATH", None)
    prev = env.get("LD_LIBRARY_PATH", "")
    env["LD_LIBRARY_PATH"] = f"{_ETCD_LIB}:{prev}" if prev else str(_ETCD_LIB)
    return env
COM_ENDPOINT = os.environ.get("THEIA_COM_ENDPOINT", "localhost:7700")

# TraceKind ordinals (services/log + platform_runtime TraceKind — aligned).
TK_STATEM = 5
# A named message-type filter so the read-back shows a concrete entry
# (msg_type="" would read back as the all-types entry, also valid — we use
# a name to prove the (node, msg_type, kind) tuple round-trips intact).
SM_MSG_TYPE = "SmStateMsg"
# The supervisor keys trace config by the WORKER/CHILD name (executor.json
# `sm`, spawned as bin/sm), NOT the node's kNodeName ("sm_daemon").
# find_worker_by_name("sm") resolves the worker, then push_trace_config_to_child
# maps to the worker's first reporting node's TIPC addr (#361). Passing
# "sm_daemon" stores config the supervisor can never push ("no worker named
# 'sm_daemon'"), so the target_node IS the child name here.
SM_TARGET_NODE = "sm"


@library(scope="SUITE")
class TraceCrashInvestigationLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self) -> None:
        self._sup: subprocess.Popen | None = None
        self._com: subprocess.Popen | None = None
        self._sup_log: Path | None = None
        self._com_log: Path | None = None

    # ----- staging + lifecycle ----------------------------------------

    @keyword("Stage And Start Central")
    def stage_and_start_central(self) -> None:
        """Lay out install/central and launch the supervisor. No boot-time
        THEIA_TRACE here — the whole point is that tracing is armed LIVE via
        the control path, then survives the crash."""
        try:
            socket.socket(socket.AF_TIPC, socket.SOCK_DGRAM).close()
        except OSError as e:
            raise AssertionError(f"AF_TIPC unavailable ({e}); modprobe tipc")

        env = _stack_env()
        r = subprocess.run(["bash", "apps/stage_local.sh"], cwd=str(_WS),
                           env=env, capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError(f"stage_local.sh failed:\n{r.stdout}\n{r.stderr}")

        self._sup_log = Path(f"/tmp/rf_crashtr_sup_{os.getpid()}.log")
        env["THEIA_INSTALL_DIR"] = str(CENTRAL_DIR / "current")
        self._sup = subprocess.Popen(
            ["./supervisor", "run", "executor.json",
             "--root-dir", ".", "--machine-name", "central_host"],
            cwd=str(CENTRAL_DIR),
            stdout=open(self._sup_log, "w"), stderr=subprocess.STDOUT,
            env=env, preexec_fn=os.setsid)
        self._wait_log(self._sup_log, "[sm_daemon] up", 8.0, "sm to bind")

    @keyword("Start Com Bridge")
    def start_com_bridge(self, timeout: float = 8.0) -> None:
        """Launch the com gRPC bridge (after the supervisor — it connects to
        the supervisor's TIPC at startup)."""
        if not COM_BRIDGE.exists():
            raise AssertionError(
                f"com binary not built at {COM_BRIDGE} — "
                f"bazel build //services/com/main:com")
        self._com_log = Path(f"/tmp/rf_crashtr_com_{os.getpid()}.log")
        env = _stack_env()
        # The native com binary takes NO argv; the gRPC listen addr comes
        # from $THEIA_COM_LISTEN (default 0.0.0.0:7700).
        env["THEIA_COM_LISTEN"] = "0.0.0.0:7700"
        self._com = subprocess.Popen(
            [str(COM_BRIDGE)],
            stdout=open(self._com_log, "w"), stderr=subprocess.STDOUT,
            env=env, preexec_fn=os.setsid)
        self._wait_tcp("localhost:7700", timeout, self._com, self._com_log,
                       "com gRPC")

    # ----- set + read back --------------------------------------------

    @keyword("Activate Sm Statem Trace Via Com")
    def activate_sm_trace(self) -> None:
        """rf → com.ConfigureTrace(sm_daemon, SmStateMsg, TK_STATEM) →
        supervisor. Asserts the ControlReply was accepted."""
        from client import Client
        c = Client(COM_ENDPOINT)
        try:
            reply = c.configure_trace(SM_TARGET_NODE, SM_MSG_TYPE,
                                      enabled=True, kind=TK_STATEM)
        finally:
            c.close()
        if getattr(reply, "status", 0) != 0:
            raise AssertionError(
                f"ConfigureTrace refused: status={reply.status} "
                f"msg={getattr(reply, 'message', '')}")

    @keyword("Read Back Trace Config From Supervisor")
    def read_back_trace_config(self) -> list:
        """rf → com.GetTraceConfig → supervisor (op_kind=10). Returns the
        list of {target_node, msg_type, enabled, kind} the supervisor holds."""
        from client import Client
        c = Client(COM_ENDPOINT)
        try:
            return c.get_trace_config()
        finally:
            c.close()

    @keyword("Trace Config Should Contain Sm Statem")
    def assert_sm_statem_present(self, configs: list) -> None:
        """Assert the read-back list carries the sm/SmStateMsg/TK_STATEM
        entry — i.e. the supervisor really remembers what we armed."""
        for c in configs:
            if (c["target_node"] == SM_TARGET_NODE
                    and c["msg_type"] == SM_MSG_TYPE
                    and c["kind"] == TK_STATEM
                    and c["enabled"]):
                return
        raise AssertionError(
            f"supervisor read-back missing sm/{SM_MSG_TYPE}/TK_STATEM; "
            f"got: {configs}")

    # ----- crash + re-apply -------------------------------------------

    @keyword("Wait For Supervisor Push To Sm")
    def wait_for_push(self, timeout: float = 5.0) -> None:
        """The supervisor's push log is written asynchronously after
        ConfigureTrace returns. Poll for at least one push entry."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self._count_pushes() >= 1:
                return
            time.sleep(0.1)
        raise AssertionError(
            f"supervisor logged no trace-config push to a child within "
            f"{timeout}s after ConfigureTrace. Supervisor log:\n"
            f"{self._tail(self._sup_log)}")

    @keyword("Note Trace Push Count")
    def note_push_count(self) -> int:
        """How many times the supervisor has logged a trace-config PUSH so
        far. The re-apply assertion checks this strictly increases after the
        crash (a SECOND push to the restarted child)."""
        return self._count_pushes()

    @keyword("Crash The Sm Child")
    def crash_sm(self) -> None:
        """SIGKILL the sm child PROCESS (not a graceful RestartChild) — a
        real crash. The supervisor's restart strategy respawns it. We learn
        sm's pid by name from the process table (the supervisor spawned it as
        the staged bin/sm)."""
        pid = self._find_sm_pid()
        if pid is None:
            raise AssertionError("could not find a running sm child to crash")
        os.kill(pid, signal.SIGKILL)
        # Wait for the supervisor to notice + respawn: a fresh "[sm_daemon] up"
        # OR a new pid distinct from the one we killed.
        deadline = time.time() + 12.0
        while time.time() < deadline:
            new = self._find_sm_pid()
            if new is not None and new != pid:
                return
            if self._sup and self._sup.poll() is not None:
                raise AssertionError(
                    f"supervisor exited during sm restart:\n"
                    f"{self._sup_log.read_text()}")
            time.sleep(0.2)
        raise AssertionError(
            f"sm did not restart with a new pid within 12s (killed {pid}); "
            f"supervisor log:\n{self._tail(self._sup_log)}")

    @keyword("Supervisor Reapplied Trace After Restart")
    def supervisor_reapplied(self, baseline: int, timeout: float = 12.0) -> None:
        """Assert the supervisor logged a NEW trace-config push (count >
        baseline) after the crash — the heartbeat-after-gap re-push (#361)
        that re-arms the freshly-restarted sm child."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self._count_pushes() > baseline:
                return
            time.sleep(0.2)
        raise AssertionError(
            f"supervisor never re-pushed trace config after sm restart "
            f"(push count stayed at {baseline}); the heartbeat-after-gap "
            f"re-apply (#361) did not fire. Supervisor log:\n"
            f"{self._tail(self._sup_log)}")

    # ----- teardown ---------------------------------------------------

    @keyword("Stop Crash Investigation Stack")
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

    def _count_pushes(self) -> int:
        """Count supervisor 'pushed N trace config entries to ...' lines."""
        if not (self._sup_log and self._sup_log.exists()):
            return 0
        return len(re.findall(r"pushed \d+ trace config entries",
                              self._sup_log.read_text()))

    def _find_sm_pid(self) -> int | None:
        """Find the running sm child by its staged binary path. The
        supervisor spawns CENTRAL_DIR/bin/sm; match on that argv so we don't
        hit the supervisor or com."""
        needle = "bin/sm"
        try:
            out = subprocess.run(["pgrep", "-f", needle],
                                 capture_output=True, text=True)
        except FileNotFoundError:
            raise AssertionError("pgrep not available on this host")
        pids = [int(x) for x in out.stdout.split() if x.strip().isdigit()]
        # Exclude our own supervisor / com pids defensively.
        own = {p.pid for p in (self._sup, self._com) if p is not None}
        pids = [p for p in pids if p not in own]
        return pids[0] if pids else None

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

    def _tail(self, path: Path | None, n: int = 1800) -> str:
        if not (path and path.exists()):
            return "(no log)"
        return path.read_text()[-n:]

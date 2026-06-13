"""Robot library for the two-supervisor central/compute selftest.

Drives a LIVE 2-machine demo on a single host: two `supervisor`
processes, each fed its own per-machine ``executor.json`` emitted by
``artheia executor emit --rig {Central,Compute}Rig``. Central hosts the
4 FC daemons (sm/com/per/ucm) + apps p1/p2; compute hosts the shwa FC +
app p3. p3's DriverNode is a cross-process consumer of p1's CounterNode,
so a green run proves the .art-declared cross-machine TIPC wiring holds
when the two supervisor trees are brought up independently.

Layout is produced by ``apps/stage_local.sh`` (install/<machine>/ with
supervisor + executor.json + bin/<child>). This lib only orchestrates:
stage → launch central → launch compute → assert spawns → SIGTERM →
assert clean exit. No Puppet/systemd — the supervisor binary is run
directly so the loop stays fast and hermetic.

Only used by two_supervisor_selftest.robot.
"""
from __future__ import annotations

import os
import re
import signal
import subprocess
import time
from pathlib import Path

from robot.api.deco import keyword, library


@library(scope="SUITE")
class TwoSupervisorLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    # start_cmd leaves the rig pins per machine (executor.json children).
    _CENTRAL_CHILDREN = ("sm", "com", "per", "ucm", "p1", "p2")
    _COMPUTE_CHILDREN = ("shwa", "p3")

    def __init__(self) -> None:
        self._workspace: Path | None = None
        # machine -> {"proc": Popen, "log": Path}
        self._supervisors: dict[str, dict] = {}

    # ------------------------------------------------------------------
    # Setup
    # ------------------------------------------------------------------

    @keyword("Use Workspace")
    def use_workspace(self, path: str) -> None:
        self._workspace = Path(path).resolve()
        if not (self._workspace / "MODULE.bazel").exists():
            raise AssertionError(
                f"{self._workspace} doesn't look like a pero_theia checkout"
            )

    @keyword("Stage Install Tree")
    def stage_install_tree(self) -> None:
        """Run apps/stage_local.sh to lay out install/{central,compute}/.

        Assumes the binaries are already built (5 FC daemons via Bazel,
        p1/p2/p3 via the demo CMake build, supervisor via CMake). The
        script copies them into per-machine bin/ dirs and emits each
        machine's executor.json."""
        assert self._workspace is not None
        env = os.environ.copy()
        env.pop("PYTHONPATH", None)
        r = subprocess.run(
            ["bash", "apps/stage_local.sh"],
            cwd=str(self._workspace), env=env,
            capture_output=True, text=True, check=False,
        )
        if r.returncode != 0:
            raise AssertionError(
                f"stage_local.sh failed (rc={r.returncode}):\n"
                f"stdout: {r.stdout}\nstderr: {r.stderr}"
            )
        for machine in ("central", "compute"):
            exe = self._workspace / "install" / machine / "executor.json"
            sup = self._workspace / "install" / machine / "supervisor"
            for p in (exe, sup):
                if not p.exists():
                    raise AssertionError(f"stage produced no {p}")

    # ------------------------------------------------------------------
    # Launch / wait
    # ------------------------------------------------------------------

    @keyword("Start Supervisor")
    def start_supervisor(self, machine: str) -> None:
        """Launch install/<machine>/supervisor against its executor.json,
        cwd'd into the machine dir so the `bin/<child>` start_cmds
        resolve. Stdout+stderr go to a per-machine log we grep later."""
        assert self._workspace is not None
        mdir = self._workspace / "install" / machine
        log_path = mdir / "supervisor.out"
        log_f = open(log_path, "w")
        proc = subprocess.Popen(
            ["./supervisor", "run", "executor.json",
             "--root-dir", ".", "--machine-name", f"{machine}_host"],
            cwd=str(mdir),
            stdout=log_f, stderr=subprocess.STDOUT,
            preexec_fn=os.setsid,   # own process group → clean group kill
        )
        self._supervisors[machine] = {
            "proc": proc, "log": log_path, "log_f": log_f,
        }

    @keyword("Wait For Child Started")
    def wait_for_child_started(self, machine: str, child: str,
                               timeout: float = 10.0) -> None:
        """Block until the supervisor's log shows it started ``child``."""
        pat = re.compile(rf"starting child {re.escape(child)}\b")
        self._wait_for_pattern(machine, pat, timeout,
                               f"child {child!r} to start on {machine}")

    @keyword("Wait For Log Pattern")
    def wait_for_log_pattern(self, machine: str, pattern: str,
                             timeout: float = 10.0) -> None:
        self._wait_for_pattern(machine, re.compile(pattern), timeout,
                               f"/{pattern}/ on {machine}")

    def _wait_for_pattern(self, machine: str, pat: re.Pattern,
                          timeout: float, what: str) -> None:
        log_path = self._supervisors[machine]["log"]
        deadline = time.time() + float(timeout)
        while time.time() < deadline:
            if log_path.exists() and pat.search(log_path.read_text()):
                return
            if self._exited_abnormally(machine):
                raise AssertionError(
                    f"{machine} supervisor exited before {what}:\n"
                    f"{log_path.read_text()}"
                )
            time.sleep(0.1)
        raise AssertionError(
            f"timed out after {timeout}s waiting for {what}:\n"
            f"{log_path.read_text()}"
        )

    def _exited_abnormally(self, machine: str) -> bool:
        proc = self._supervisors[machine]["proc"]
        rc = proc.poll()
        return rc is not None and rc != 0

    # ------------------------------------------------------------------
    # Assertions
    # ------------------------------------------------------------------

    @keyword("All Central Children Started")
    def all_central_children_started(self, timeout: float = 12.0) -> None:
        for c in self._CENTRAL_CHILDREN:
            self.wait_for_child_started("central", c, timeout)

    @keyword("All Compute Children Started")
    def all_compute_children_started(self, timeout: float = 12.0) -> None:
        for c in self._COMPUTE_CHILDREN:
            self.wait_for_child_started("compute", c, timeout)

    @keyword("P3 Reached Counter On Central")
    def p3_reached_counter(self, timeout: float = 15.0) -> None:
        """p3 (compute) talks to p1's CounterNode (central) over TIPC.
        A successful round shows `P3 summary: casts_sent=N` with N>0 and
        a normal (code=0) exit — not the `failed to connect to
        CounterNode` error seen when p1 isn't up."""
        log_path = self._supervisors["compute"]["log"]
        deadline = time.time() + float(timeout)
        good = re.compile(r"P3 summary: casts_sent=(\d+)")
        bad = re.compile(r"failed to connect to CounterNode")
        while time.time() < deadline:
            txt = log_path.read_text() if log_path.exists() else ""
            m = good.search(txt)
            if m and int(m.group(1)) > 0:
                return
            time.sleep(0.1)
        txt = log_path.read_text() if log_path.exists() else ""
        hint = " (saw connect failure)" if bad.search(txt) else ""
        raise AssertionError(
            f"p3 never reached p1's CounterNode within {timeout}s{hint}:\n"
            f"{txt}"
        )

    @keyword("Supervisor Log Contains")
    def supervisor_log_contains(self, machine: str, needle: str) -> None:
        txt = self._supervisors[machine]["log"].read_text()
        if needle not in txt:
            raise AssertionError(
                f"{machine} log missing {needle!r}:\n{txt}"
            )

    # ------------------------------------------------------------------
    # Teardown
    # ------------------------------------------------------------------

    @keyword("Stop Supervisor")
    def stop_supervisor(self, machine: str, timeout: float = 8.0) -> None:
        """SIGTERM the supervisor's process group and assert it drains
        (OTP-style: stops children in reverse start order, then exits)."""
        entry = self._supervisors.get(machine)
        if not entry:
            return
        proc = entry["proc"]
        if proc.poll() is None:
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
                proc.wait(timeout=2.0)
                raise AssertionError(
                    f"{machine} supervisor didn't exit on SIGTERM within "
                    f"{timeout}s; SIGKILLed.\n{entry['log'].read_text()}"
                )
        entry["log_f"].close()

    @keyword("Stop All Supervisors")
    def stop_all_supervisors(self) -> None:
        """Reverse-order teardown: compute (depends on central) first."""
        for machine in ("compute", "central"):
            if machine in self._supervisors:
                try:
                    self.stop_supervisor(machine)
                except AssertionError:
                    # Best-effort during suite teardown; re-raise only
                    # outside teardown via Stop Supervisor directly.
                    pass

    @keyword("Supervisor Exited Cleanly")
    def supervisor_exited_cleanly(self, machine: str) -> None:
        proc = self._supervisors[machine]["proc"]
        rc = proc.poll()
        if rc is None:
            raise AssertionError(f"{machine} supervisor still running")
        # SIGTERM → graceful shutdown path logs "supervisor exiting" and
        # returns 0. (128+15=143 would mean it died TO the signal rather
        # than handling it — that's a failure for this supervisor.)
        if rc != 0:
            raise AssertionError(
                f"{machine} supervisor exit code {rc} (expected 0):\n"
                f"{self._supervisors[machine]['log'].read_text()}"
            )
        self.supervisor_log_contains(machine, "supervisor exiting")

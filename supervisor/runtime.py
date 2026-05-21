"""Supervisor runtime: process tree + restart semantics.

The supervisor is a single Python process. The supervisor *tree* is
modelled internally — supervisors-of-supervisors are bookkeeping, not
separate OS processes. Only :class:`ChildSpec` workers fork. This
mirrors how OTP supervisors compose in one BEAM VM: nested supervisors
own their children's lifecycle without forking the supervisor itself.

Restart semantics:

- The supervisor maintains, per :class:`SupervisorNode`, a sliding
  window of restart timestamps. If ``len > max_restarts`` within
  ``max_seconds``, the supervisor *escalates*: it terminates all its
  children and tells its parent (``shutdown_signal`` propagates).
- Worker exits are observed via :func:`os.waitpid(-1, os.WNOHANG)` in
  the main loop. ``signal.set_wakeup_fd`` plus a ``select.select`` is
  used so SIGCHLD interrupts the loop deterministically without races.
"""

from __future__ import annotations

import logging
import os
import select
import signal
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

log = logging.getLogger("supervisor")


# ---------------------------------------------------------------------------
# Internal tree representation
# ---------------------------------------------------------------------------


@dataclass
class WorkerNode:
    """One leaf child (a forked POSIX process)."""

    name: str
    start_cmd: list[str]
    restart: str = "permanent"      # permanent | transient | temporary
    shutdown: Any = 5000            # ms | "brutal_kill" | "infinity"
    env: dict[str, str] = field(default_factory=dict)
    working_dir: str = ""
    modules: list[str] = field(default_factory=list)

    # Runtime state:
    # CPU affinity, AUTOSAR-flavoured (ProcessToMachineMapping in §9.4).
    # Mutually exclusive: shall_run_on (positive) or shall_not_run_on
    # (negative). Empty means no constraint.
    shall_run_on: list[int] = field(default_factory=list)
    shall_not_run_on: list[int] = field(default_factory=list)

    proc: subprocess.Popen | None = None
    last_start: float = 0.0
    # True while stop_worker() is actively terminating this worker. When
    # set, _reap() skips _on_child_exit() for this worker — the
    # synchronous stop path owns the wait. Mirrors OTP's
    # ``{restarting, OldPid}`` sentinel; prevents an in-flight shutdown's
    # SIGCHLD from triggering a spurious restart.
    terminating: bool = False

    @property
    def pid(self) -> int | None:
        return self.proc.pid if self.proc else None

    @property
    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None


@dataclass
class SupervisorNode:
    """One supervisor in the tree (bookkeeping; not a forked process)."""

    name: str
    strategy: str = "one_for_one"   # one_for_one | one_for_all | rest_for_one | simple_one_for_one
    max_restarts: int = 3
    max_seconds: int = 5
    children: list["WorkerNode | SupervisorNode"] = field(default_factory=list)
    parent: "SupervisorNode | None" = None
    # Project extension: where to look for libtombstone tombstones when
    # a descendant child dies from a fatal signal. Set on the root by
    # convention; leaves inherit by walking parent links.
    tombstone_dir: str = ""

    # Sliding-window restart history (epoch seconds).
    restart_history: list[float] = field(default_factory=list)

    # ----- helpers ----------------------------------------------------------

    def all_workers(self) -> list[WorkerNode]:
        """Flatten the subtree into the list of leaf workers."""
        out: list[WorkerNode] = []
        for child in self.children:
            if isinstance(child, WorkerNode):
                out.append(child)
            else:
                out.extend(child.all_workers())
        return out

    def child_of(self, worker: WorkerNode) -> bool:
        """True if ``worker`` lives anywhere in this subtree."""
        return any(w is worker for w in self.all_workers())

    def supervisor_of(self, worker: WorkerNode) -> "SupervisorNode | None":
        """Innermost supervisor owning ``worker`` (None if not in tree)."""
        for child in self.children:
            if child is worker:
                return self
            if isinstance(child, SupervisorNode):
                hit = child.supervisor_of(worker)
                if hit is not None:
                    return hit
        return None


# ---------------------------------------------------------------------------
# Tree loading
# ---------------------------------------------------------------------------


def _load_node(d: dict, parent: SupervisorNode | None = None) -> WorkerNode | SupervisorNode:
    if "children" in d:
        node = SupervisorNode(
            name=d["name"],
            strategy=d.get("strategy", "one_for_one"),
            max_restarts=d.get("max_restarts", 3),
            max_seconds=d.get("max_seconds", 5),
            parent=parent,
            tombstone_dir=d.get("tombstone_dir", ""),
        )
        node.children = [_load_node(c, node) for c in d["children"]]
        return node
    shall_run_on = list(d.get("shall_run_on", []))
    shall_not_run_on = list(d.get("shall_not_run_on", []))
    if shall_run_on and shall_not_run_on:
        raise ValueError(
            f"child '{d['name']}': shall_run_on and shall_not_run_on are mutually exclusive"
        )
    return WorkerNode(
        name=d["name"],
        start_cmd=list(d["start_cmd"]),
        restart=d.get("restart", "permanent"),
        shutdown=d.get("shutdown", 5000),
        env=dict(d.get("env", {})),
        working_dir=d.get("working_dir", ""),
        modules=list(d.get("modules", [])),
        shall_run_on=shall_run_on,
        shall_not_run_on=shall_not_run_on,
    )


# ---------------------------------------------------------------------------
# Supervisor
# ---------------------------------------------------------------------------


class Supervisor:
    """Runs the tree."""

    def __init__(self, manifest: dict, root_dir: Path) -> None:
        node = _load_node(manifest)
        if not isinstance(node, SupervisorNode):
            raise ValueError("manifest root must be a supervisor (have 'children')")
        self.root = node
        self.root_dir = root_dir
        self._shutdown_requested = False
        # Non-zero exit if the root supervisor blew through its restart
        # budget: the init system should treat this as a failure.
        self._escalated = False

        # Wakeup pipe so SIGCHLD interrupts select().
        self._wake_rfd, self._wake_wfd = os.pipe()
        os.set_blocking(self._wake_rfd, False)
        os.set_blocking(self._wake_wfd, False)
        signal.set_wakeup_fd(self._wake_wfd)
        signal.signal(signal.SIGCHLD, lambda *_: None)  # arm SIGCHLD delivery

    # ----- public API -------------------------------------------------------

    def request_shutdown(self, signum=None, frame=None) -> None:
        log.info("shutdown requested (signal=%s)", signum)
        self._shutdown_requested = True

    def run(self) -> int:
        log.info("supervisor starting (root=%s)", self.root.name)
        self._start_subtree(self.root)
        try:
            self._loop()
        finally:
            self._shutdown_subtree(self.root)
            log.info("supervisor exiting")
        return 1 if self._escalated else 0

    # ----- start / stop primitives ------------------------------------------

    def _start_worker(self, w: WorkerNode) -> None:
        cmd = list(w.start_cmd)
        # Resolve relative paths against root_dir.
        if cmd and not os.path.isabs(cmd[0]):
            candidate = self.root_dir / cmd[0]
            if candidate.exists():
                cmd[0] = str(candidate)

        env = dict(os.environ)
        env.update(w.env)
        cwd = w.working_dir or str(self.root_dir)

        log.info("starting child %s: %s", w.name, " ".join(cmd))
        # preexec_fn runs in the child between fork and exec. We use it to:
        # 1) setsid() so the child leads its own process group (clean group
        #    kills), and
        # 2) apply CPU affinity per shall_run_on / shall_not_run_on.
        shall_run_on = list(w.shall_run_on)
        shall_not_run_on = list(w.shall_not_run_on)

        def _child_setup() -> None:
            os.setsid()
            if shall_run_on:
                os.sched_setaffinity(0, set(shall_run_on))
            elif shall_not_run_on:
                online = os.sched_getaffinity(0)
                os.sched_setaffinity(0, online - set(shall_not_run_on))

        try:
            w.proc = subprocess.Popen(
                cmd,
                env=env,
                cwd=cwd,
                preexec_fn=_child_setup,
            )
        except (FileNotFoundError, PermissionError, OSError) as e:
            log.error("cannot start %s: %s", w.name, e)
            w.proc = None
            # Treat start failure the same way the C++ port does: the child
            # would have exited 127 from the fork() path, hitting the
            # normal SIGCHLD/reap pipeline. Funnel that through
            # _on_child_exit so restart intensity is consumed and the
            # supervisor escalates when the budget runs out — instead of
            # silently giving up after one log line.
            self._on_child_exit(w, 127)
            return
        w.last_start = time.time()

    def _start_subtree(self, sup: SupervisorNode) -> None:
        for child in sup.children:
            if isinstance(child, WorkerNode):
                self._start_worker(child)
            else:
                self._start_subtree(child)

    def _stop_worker(self, w: WorkerNode) -> None:
        if not w.alive:
            return
        log.info("stopping %s (pid=%s)", w.name, w.pid)
        # Mark terminating before issuing the signal so _reap() doesn't
        # try to restart this worker if our own SIGCHLD races with the
        # synchronous wait below. Matches the C++ port and OTP's
        # {restarting, OldPid} sentinel.
        w.terminating = True
        try:
            if w.shutdown == "brutal_kill":
                os.killpg(w.proc.pid, signal.SIGKILL)
            else:
                os.killpg(w.proc.pid, signal.SIGTERM)

            if w.shutdown == "infinity":
                w.proc.wait()
            elif w.shutdown == "brutal_kill":
                w.proc.wait(timeout=2)
            else:
                timeout_s = max(0.1, float(w.shutdown) / 1000.0)
                try:
                    w.proc.wait(timeout=timeout_s)
                except subprocess.TimeoutExpired:
                    log.warning("SIGTERM timed out for %s, SIGKILLing", w.name)
                    os.killpg(w.proc.pid, signal.SIGKILL)
                    w.proc.wait(timeout=2)
        except (ProcessLookupError, PermissionError) as e:
            log.warning("stop %s: %s", w.name, e)
        w.proc = None
        w.terminating = False

    def _shutdown_subtree(self, sup: SupervisorNode) -> None:
        # Stop workers in reverse declared order so dependents go first.
        for child in reversed(sup.children):
            if isinstance(child, WorkerNode):
                self._stop_worker(child)
            else:
                self._shutdown_subtree(child)

    # ----- tombstone surfacing ---------------------------------------------

    def _find_tombstone_dir(self, sup: SupervisorNode) -> str:
        """Walk up to the root looking for a configured tombstone_dir."""
        n: SupervisorNode | None = sup
        while n is not None:
            if n.tombstone_dir:
                return n.tombstone_dir
            n = n.parent
        return ""

    def _locate_tombstone(self, directory: str, name: str, pid: int) -> str:
        """Newest tombstone matching prefix tombstone-<name>-<pid>-."""
        import glob
        prefix = f"tombstone-{name}-{pid}-"
        candidates = glob.glob(os.path.join(directory, prefix + "*"))
        if not candidates:
            return ""
        return max(candidates, key=lambda p: os.stat(p).st_mtime)

    # ----- restart policy ---------------------------------------------------

    def _on_child_exit(self, w: WorkerNode, returncode: int, old_pid: int = -1) -> None:
        sup = self.root.supervisor_of(w)
        if sup is None:
            log.warning("exit from unknown child %s (pid=%s)", w.name, w.pid)
            return

        abnormal = returncode != 0
        log.info(
            "child %s exited (code=%d, %s) — sup=%s strategy=%s",
            w.name, returncode, "abnormal" if abnormal else "normal",
            sup.name, sup.strategy,
        )

        # Tombstone surfacing: signal-induced exits (rc < 0) may have a
        # tombstone written by libtombstone. Walk to the root supervisor
        # to find the configured directory; pick the newest tombstone for
        # this worker's pid.
        if returncode < 0 and old_pid > 0:
            ts_dir = self._find_tombstone_dir(sup)
            if ts_dir:
                ts = self._locate_tombstone(ts_dir, w.name, old_pid)
                if ts:
                    log.error("tombstone for %s (pid=%d): %s",
                              w.name, old_pid, ts)

        # Honour restart type.
        if w.restart == "temporary":
            return
        if w.restart == "transient" and not abnormal:
            return

        # Bounded-restart check before doing anything.
        if not self._record_and_check_restart(sup):
            log.error(
                "supervisor %s exceeded restart intensity (%d in %ds) — escalating",
                sup.name, sup.max_restarts, sup.max_seconds,
            )
            self._escalated = True
            self._shutdown_requested = True
            return

        # Apply strategy.
        if sup.strategy == "one_for_one":
            self._start_worker(w)
        elif sup.strategy == "one_for_all":
            self._restart_all(sup)
        elif sup.strategy == "rest_for_one":
            self._restart_rest(sup, w)
        elif sup.strategy == "simple_one_for_one":
            log.warning("simple_one_for_one not implemented; treating as one_for_one")
            self._start_worker(w)
        else:
            log.error("unknown strategy %s on %s", sup.strategy, sup.name)

    def _record_and_check_restart(self, sup: SupervisorNode) -> bool:
        now = time.time()
        cutoff = now - sup.max_seconds
        sup.restart_history = [t for t in sup.restart_history if t >= cutoff]
        sup.restart_history.append(now)
        return len(sup.restart_history) <= sup.max_restarts

    def _restart_all(self, sup: SupervisorNode) -> None:
        log.info("one_for_all: restarting all of sup=%s", sup.name)
        # Stop in reverse declared order, restart in forward order.
        workers = sup.all_workers()
        for w in reversed(workers):
            self._stop_worker(w)
        for w in workers:
            self._start_worker(w)

    def _restart_rest(self, sup: SupervisorNode, failed: WorkerNode) -> None:
        # In rest_for_one: restart the failed child and everything declared
        # *after* it in this supervisor's direct children list. Children that
        # are themselves SupervisorNodes contribute their whole subtree.
        log.info("rest_for_one: restarting %s and downstream in sup=%s", failed.name, sup.name)
        seen = False
        affected: list[WorkerNode] = []
        for child in sup.children:
            if not seen:
                if isinstance(child, WorkerNode) and child is failed:
                    seen = True
                    affected.append(child)
                elif isinstance(child, SupervisorNode) and child.child_of(failed):
                    seen = True
                    affected.extend(child.all_workers())
                continue
            if isinstance(child, WorkerNode):
                affected.append(child)
            else:
                affected.extend(child.all_workers())

        # Stop in reverse, restart forward.
        for w in reversed(affected):
            self._stop_worker(w)
        for w in affected:
            self._start_worker(w)

    # ----- main loop --------------------------------------------------------

    def _loop(self) -> None:
        while not self._shutdown_requested:
            # Drain wakeup-fd noise.
            try:
                select.select([self._wake_rfd], [], [], 1.0)
                try:
                    os.read(self._wake_rfd, 4096)
                except BlockingIOError:
                    pass
            except InterruptedError:
                pass

            # Reap any exited workers.
            self._reap()

    def _reap(self) -> None:
        all_workers = self.root.all_workers()
        by_pid = {w.pid: w for w in all_workers if w.pid is not None}
        while True:
            try:
                pid, status = os.waitpid(-1, os.WNOHANG)
            except ChildProcessError:
                return
            if pid == 0:
                return
            w = by_pid.get(pid)
            if w is None:
                continue
            # Translate exit status.
            if os.WIFEXITED(status):
                rc = os.WEXITSTATUS(status)
            elif os.WIFSIGNALED(status):
                rc = -os.WTERMSIG(status)
            else:
                rc = -1
            # Mark dead so .alive returns False before we re-fork.
            was_terminating = w.terminating
            w.proc = None
            if was_terminating:
                # _stop_worker() owns this exit; just acknowledge.
                log.info("child %s stopped (terminating path)", w.name)
                continue
            self._on_child_exit(w, rc, old_pid=pid)

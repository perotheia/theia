"""Mender → UCM hand-off tests (native adopter).

After the theia-release module lands a release + flips `current`, the
ArtifactInstall_Leave state-script hands the release to the on-device UCM agent
via `theia-migrate adopt` (UcmDaemon.RequestUpdate over the runtime's RemoteRef —
native C++, no python/artheia probe on the rig; the retired ucm-adopt.py python
driver used the probe and was deleted).

The LIVE hand-off (RequestUpdate → FSM → VALIDATED→STAGED) is verified against a
running UCM on a rig. Here we test the GRACEFUL-DEGRADATION contract a
state-script depends on: it must exit 0 (the symlink switch IS the install) when
no UCM / no adopter is present, so a standalone `mender install` never fails for
lack of a running agent.
"""
import os
import subprocess

from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
STATE_SCRIPT = (REPO / "platform" / "runtime" / "ota" / "state-scripts"
                / "ArtifactInstall_Leave_01_ucm-request")


def test_state_script_degrades_without_adopter(tmp_path):
    """No $THEIA_ROOT/bin/theia-migrate (bare box) → exit 0 with a current release.
    The symlink switch is the install; the supervisor adopts `current` on restart."""
    root = tmp_path / "opt-theia"
    (root / "releases" / "1.0.0").mkdir(parents=True)
    os.symlink("releases/1.0.0", root / "current")
    env = {**os.environ, "THEIA_ROOT": str(root)}
    r = subprocess.run(["sh", str(STATE_SCRIPT)], env=env,
                       capture_output=True, text=True)
    assert r.returncode == 0, f"hook failed:\n{r.stdout}\n{r.stderr}"
    assert "symlink-only install" in (r.stdout + r.stderr)


def test_state_script_fails_with_no_current(tmp_path):
    """No `current` symlink → the hook errors (a release must be landed first)."""
    root = tmp_path / "opt-theia"
    root.mkdir()
    env = {**os.environ, "THEIA_ROOT": str(root)}
    r = subprocess.run(["sh", str(STATE_SCRIPT)], env=env,
                       capture_output=True, text=True)
    assert r.returncode != 0


def test_state_script_invokes_native_adopter(tmp_path):
    """When $THEIA_ROOT/bin/theia-migrate exists, the hook calls it with
    `adopt <ver> full`. A stub records the argv; the hook must pass it through and
    honor its exit code."""
    root = tmp_path / "opt-theia"
    (root / "releases" / "2.3.0").mkdir(parents=True)
    (root / "bin").mkdir()
    os.symlink("releases/2.3.0", root / "current")
    stub = root / "bin" / "theia-migrate"
    argv_log = tmp_path / "argv.txt"
    stub.write_text('#!/bin/sh\nprintf "%%s\\n" "$@" > "%s"\nexit 0\n' % argv_log)
    stub.chmod(0o755)
    env = {**os.environ, "THEIA_ROOT": str(root)}
    r = subprocess.run(["sh", str(STATE_SCRIPT)], env=env,
                       capture_output=True, text=True)
    assert r.returncode == 0, f"{r.stdout}\n{r.stderr}"
    argv = argv_log.read_text().split()
    assert argv[:3] == ["adopt", "2.3.0", "full"], argv


def test_state_script_propagates_ucm_reject(tmp_path):
    """An explicit UCM reject (adopter exit 1) must fail the install → Mender
    ArtifactRollback."""
    root = tmp_path / "opt-theia"
    (root / "releases" / "2.3.0").mkdir(parents=True)
    (root / "bin").mkdir()
    os.symlink("releases/2.3.0", root / "current")
    stub = root / "bin" / "theia-migrate"
    stub.write_text("#!/bin/sh\nexit 1\n")
    stub.chmod(0o755)
    env = {**os.environ, "THEIA_ROOT": str(root)}
    r = subprocess.run(["sh", str(STATE_SCRIPT)], env=env,
                       capture_output=True, text=True)
    assert r.returncode != 0

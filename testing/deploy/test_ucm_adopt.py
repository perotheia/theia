"""ucm-adopt.py tests — the Mender → UCM hand-off bridge.

After the theia-release module lands a release + flips `current`, the
ArtifactInstall_Leave state-script calls ucm-adopt.py to hand the release to the
on-device UCM agent (UcmDaemon.RequestUpdate over the artheia probe). The LIVE
hand-off (probe → FSM → VALIDATED→STAGED) was verified against a running UCM; here
we test the GRACEFUL-DEGRADATION contract that a state-script depends on — it must
exit 0 (the symlink switch IS the install) when UCM/artheia isn't present, so a
standalone `mender install` never fails for lack of a running agent.
"""
import os
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
ADOPT = REPO / "deploy" / "mender" / "state-scripts" / "ucm-adopt.py"
STATE_SCRIPT = REPO / "deploy" / "mender" / "state-scripts" / "ArtifactInstall_Leave_01_ucm-request"


def _run_adopt(version, theia_root, extra_env=None):
    env = {**os.environ, "THEIA_ROOT": str(theia_root)}
    if extra_env:
        env.update(extra_env)
    return subprocess.run([sys.executable, str(ADOPT), version, "full"],
                          env=env, capture_output=True, text=True)


def test_adopt_noops_when_no_artheia(tmp_path):
    """No artheia under THEIA_ROOT → exit 0 (symlink-only install), never fail."""
    root = tmp_path / "opt-theia"
    root.mkdir()
    r = _run_adopt("1.0.0", root)
    assert r.returncode == 0, r.stderr
    assert "symlink-only" in (r.stdout + r.stderr).lower()


def test_adopt_noops_when_tdb_art_missing(tmp_path):
    """artheia dir present but no tdb.art → still graceful exit 0."""
    root = tmp_path / "opt-theia"
    (root / "artheia").mkdir(parents=True)
    r = _run_adopt("1.0.0", root)
    assert r.returncode == 0, r.stderr


def test_adopt_passes_current_as_artifact_path(tmp_path):
    """The adopter targets the already-landed release (current), not a download
    tarball — verify the script references the current symlink as artifact_path."""
    txt = ADOPT.read_text()
    assert 'ROOT / "current"' in txt
    assert "RequestUpdate" in txt
    # UK_SOFTWARE / US_FULL mapping is what a Mender artifact always is
    assert "uk = 0" in txt  # software


def test_state_script_resolves_adopter_and_degrades(tmp_path):
    """The ArtifactInstall_Leave hook: no UCM env → exit 0 with a current release."""
    root = tmp_path / "opt-theia"
    (root / "releases" / "1.0.0").mkdir(parents=True)
    os.symlink("releases/1.0.0", root / "current")
    env = {**os.environ, "THEIA_ROOT": str(root)}
    r = subprocess.run(["sh", str(STATE_SCRIPT)], env=env,
                       capture_output=True, text=True)
    assert r.returncode == 0, f"hook failed:\n{r.stdout}\n{r.stderr}"


def test_state_script_fails_with_no_current(tmp_path):
    """No `current` symlink → the hook errors (a release must be landed first)."""
    root = tmp_path / "opt-theia"
    root.mkdir()
    env = {**os.environ, "THEIA_ROOT": str(root)}
    r = subprocess.run(["sh", str(STATE_SCRIPT)], env=env,
                       capture_output=True, text=True)
    assert r.returncode != 0

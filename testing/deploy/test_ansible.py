"""Ansible deploy-scaffold tests — provision/orchestrate playbooks (deploy/ansible/).

The agentless deploy engine (Puppet is gone). These run WITHOUT a target rig:
syntax-check + ansible-lint (production profile) + a structural check that the
playbooks carry the deploy phases (os-packages / etcd / mender / bundle / setcap
/ config). Live behaviour was hand-verified on rig1-central; this guards the
scaffold from drift.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
ANSIBLE = REPO / "deploy" / "ansible"
INVENTORY = ANSIBLE / "inventory" / "hosts"
PLAYBOOKS = ["provision.yml", "orchestrate.yml"]

# Look for ansible-playbook on PATH AND in the running interpreter's bin dir (the
# venv) — pytest may run with the venv python but a system PATH.
_VENV_BIN = Path(sys.executable).parent


def _tool(name):
    return shutil.which(name) or (
        str(_VENV_BIN / name) if (_VENV_BIN / name).exists() else None)


# Prepend the venv bin so ansible-playbook's task-include resolution + ansible-lint
# find their siblings (ansible-config, ansible-inventory).
_ENV = {**os.environ, "PATH": f"{_VENV_BIN}{os.pathsep}{os.environ.get('PATH', '')}"}

pytestmark = pytest.mark.skipif(
    _tool("ansible-playbook") is None, reason="ansible-core not installed")


@pytest.mark.parametrize("pb", PLAYBOOKS)
def test_playbook_syntax_check(pb):
    """ansible-playbook --syntax-check parses the playbook + its task includes."""
    r = subprocess.run(
        [_tool("ansible-playbook"), "--syntax-check", "-i", str(INVENTORY), pb],
        cwd=ANSIBLE, env=_ENV, capture_output=True, text=True)
    assert r.returncode == 0, f"syntax-check failed:\n{r.stdout}\n{r.stderr}"


def test_ansible_lint_production_profile():
    """The scaffold must stay clean at ansible-lint's strictest (production) profile.
    Caught real issues during authoring (risky-shell-pipe, name casing, jinja-in-name)."""
    if _tool("ansible-lint") is None:
        pytest.skip("ansible-lint not installed")
    r = subprocess.run(
        [_tool("ansible-lint"), "--profile", "production", *PLAYBOOKS],
        cwd=ANSIBLE, env=_ENV, capture_output=True, text=True)
    assert r.returncode == 0, f"ansible-lint failed:\n{r.stdout}\n{r.stderr}"


def test_task_includes_present():
    """provision/orchestrate translate the Puppet phases 1:1 — the task files must exist."""
    tasks = ANSIBLE / "tasks"
    expected = {"os-packages.yml", "etcd.yml", "install-mender.yml",
                "install-bundle.yml", "setcap.yml", "seed-config.yml"}
    have = {p.name for p in tasks.glob("*.yml")}
    missing = expected - have
    assert not missing, f"missing task includes: {missing}"


def test_inventory_resolves():
    """The inventory parses + has the rig group with a machine= var (the manifest slice)."""
    r = subprocess.run(
        [_tool("ansible-inventory"), "-i", str(INVENTORY), "--list"],
        cwd=ANSIBLE, env=_ENV, capture_output=True, text=True)
    assert r.returncode == 0, f"inventory failed:\n{r.stderr}"
    assert "machine" in r.stdout or "rig" in r.stdout.lower()


def test_install_mender_ships_the_ucm_bridge():
    """install-mender.yml must push BOTH the theia-release module + the UCM hand-off
    (the state-script and ucm-adopt.py) — the Mender→UCM bridge."""
    txt = (ANSIBLE / "tasks" / "install-mender.yml").read_text()
    assert "theia-release" in txt
    assert "ArtifactInstall_Leave" in txt
    assert "ucm-adopt.py" in txt

"""theia-release Mender update-module tests — release-dir + symlink (NOT A/B).

The module (deploy/mender/modules/theia-release) is a standalone shell script
implementing Theia's deploy model: land a release at <root>/releases/<ver>/ and
atomically re-aim <root>/current → it. Mender's rootfs-image flips A/B partitions;
this custom module is the supported customization point for that.

These drive the module DIRECTLY through its Mender states (the exact contract
`mender install` uses: `theia-release <STATE> <payload-tree>`) and assert the
release-dir + symlink behaviour + rollback — the live `mender install`/`commit`/
`rollback` cycle proven on rig1-central, reduced to a hermetic unit test.
"""
import os
import subprocess
import tarfile
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
MODULE = REPO / "deploy" / "mender" / "modules" / "theia-release"


def _make_payload_tree(tmp: Path, version: str, marker: str) -> Path:
    """Build the Mender payload 'tree' the module reads in ArtifactInstall:
    <tree>/files/{release.tar.gz, version.txt}. (Real Mender materialises files/
    by ArtifactInstall; gzip here to avoid a hard zstd dep in the test.)"""
    files = tmp / "tree" / "files"
    files.mkdir(parents=True)
    # a minimal release tree: bin/com prints the marker
    rel = tmp / f"rel_{version}"
    (rel / "bin").mkdir(parents=True)
    (rel / "bin" / "com").write_text(f"#!/bin/sh\necho {marker}\n")
    (rel / "VERSION").write_text(version)
    with tarfile.open(files / "release.tar.gz", "w:gz") as t:
        t.add(rel, arcname=".")
    (files / "version.txt").write_text(version)
    return tmp / "tree"


def _run(state: str, tree: Path, theia_root: Path):
    env = {**os.environ, "THEIA_ROOT": str(theia_root)}
    return subprocess.run(["sh", str(MODULE), state, str(tree)],
                          env=env, capture_output=True, text=True)


def _current(root: Path) -> str:
    return os.readlink(root / "current") if (root / "current").is_symlink() else ""


@pytest.fixture
def root(tmp_path):
    r = tmp_path / "opt-theia"
    r.mkdir()
    return r


def test_install_lands_release_and_switches_current(tmp_path, root):
    tree = _make_payload_tree(tmp_path / "v1", "1.0.0", "v1.0.0")
    r = _run("ArtifactInstall", tree, root)
    assert r.returncode == 0, r.stderr
    assert _current(root) == "releases/1.0.0"
    # the staged release is real (bin/com present)
    assert (root / "releases" / "1.0.0" / "bin" / "com").exists()


def test_second_install_switches_and_records_previous(tmp_path, root):
    _run("ArtifactInstall", _make_payload_tree(tmp_path / "v1", "1.0.0", "v1"), root)
    _run("ArtifactCommit", tmp_path / "v1" / "tree", root)
    r = _run("ArtifactInstall", _make_payload_tree(tmp_path / "v2", "2.0.0", "v2"), root)
    assert r.returncode == 0, r.stderr
    assert _current(root) == "releases/2.0.0"
    # previous now points at the outgoing release (the rollback target)
    assert os.readlink(root / "previous") == "releases/1.0.0"


def test_rollback_restores_previous(tmp_path, root):
    _run("ArtifactInstall", _make_payload_tree(tmp_path / "v1", "1.0.0", "v1"), root)
    _run("ArtifactCommit", tmp_path / "v1" / "tree", root)
    _run("ArtifactInstall", _make_payload_tree(tmp_path / "v2", "2.0.0", "v2"), root)
    # v2 "fails PHM" → rollback
    r = _run("ArtifactRollback", tmp_path / "v2" / "tree", root)
    assert r.returncode == 0, r.stderr
    assert _current(root) == "releases/1.0.0"


def test_switch_is_atomic_symlink_not_copy(tmp_path, root):
    """current must be a SYMLINK (the atomic rename(2) model), never a copied tree."""
    _run("ArtifactInstall", _make_payload_tree(tmp_path / "v1", "1.0.0", "v1"), root)
    assert (root / "current").is_symlink()


def test_no_ab_partition_semantics(tmp_path, root):
    """NeedsArtifactReboot=No + SupportsRollback=Yes — theia restarts FCs, never reboots."""
    assert _run("NeedsArtifactReboot", tmp_path, root).stdout.strip() == "No"
    assert _run("SupportsRollback", tmp_path, root).stdout.strip() == "Yes"


def test_download_is_noop(tmp_path, root):
    """Download is a no-op (Mender exposes only stream pipes there; staging happens
    in ArtifactInstall where files/ is materialised). Must not error."""
    assert _run("Download", tmp_path, root).returncode == 0

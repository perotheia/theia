"""release-swp writes the commit-gate contract (app.json + optional health hook)
into the SWP payload — the on-device theia-swp module's G1 app-process gate reads
it (platform/runtime/ota/test/test_modules.sh covers the rig side)."""
import json
import os
import sys
import tarfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import theia  # noqa: E402

PROCS = ["ingest", "link"]


def _fake_run(argv, cwd=None):
    if argv[0] == "bazel":
        return 0
    raise AssertionError(f"unexpected command {argv}")


@pytest.fixture
def wc_ws(tmp_path, monkeypatch):
    """dist/manifest as `theia manifest` lays it for a one-machine app `wc`, plus
    the bazel-bin FC binary release-swp stages as bin/<proc>."""
    mdir = tmp_path / "dist" / "manifest"
    d = mdir / "master"
    d.mkdir(parents=True)
    (mdir / "machines.json").write_text(json.dumps({"machines": ["master"]}))
    (d / "machine.json").write_text(json.dumps({"name": "master"}))
    (d / "application.json").write_text(json.dumps({"applications": [
        {"name": "wc", "host_machine": "master", "processes": PROCS}]}))
    (d / "executor.json").write_text(json.dumps({"name": "root", "children": [
        {"name": "applications_sup", "children": [
            {"name": "ingest", "start_cmd": "bin/ingest"},
            {"name": "link", "start_cmd": ["bin/link", "--mock"]}]}]}))
    (d / "execution.json").write_text(json.dumps({"processes": []}))
    exe = tmp_path / "bazel-bin" / "apps" / "wc" / "main" / "wc"
    exe.parent.mkdir(parents=True)
    exe.write_bytes(b"\x7fELF fake")
    monkeypatch.setattr(theia, "WORKSPACE", tmp_path)
    monkeypatch.setattr(theia, "_run", _fake_run)
    monkeypatch.setattr(theia, "_mender_artifact_bin", lambda: None)
    return tmp_path


def _payload(ws: Path, name="wc-0.1.0-amd64"):
    tgz = ws / "dist" / "apps" / "wc" / f"{name}.tar.gz"
    with tarfile.open(tgz) as tf:
        app = json.load(tf.extractfile("./app.json"))
        members = {m.name: m for m in tf.getmembers()}
    return app, members


def test_release_swp_writes_app_json_processes(wc_ws):
    assert theia.cmd_release_swp(["wc", "--swp-version", "0.1.0"]) == 0
    app, members = _payload(wc_ws)
    assert app["processes"] == [{"name": "ingest", "exe": "ingest"},
                                {"name": "link", "exe": "link"}]
    assert app["artifact"] == "wc-0.1.0-amd64" and app["health"] is None
    assert "gate" not in app
    assert "./bin/ingest" in members and "./health" not in members


def test_release_swp_ships_health_hook_and_gate(wc_ws, capsys):
    hook = wc_ws / "wc_health"
    hook.write_text("#!/bin/sh\nexit 0\n")
    rc = theia.cmd_release_swp(["wc", "--swp-version", "0.1.0", "--health", str(hook),
                                "--gate", "wait_s=180,stable_s=20,hook_timeout_s=99"])
    assert rc == 0
    app, members = _payload(wc_ws)
    assert app["health"] == "health"
    assert app["gate"] == {"wait_s": 180, "stable_s": 20, "hook_timeout_s": 99}
    assert members["./health"].mode & 0o111
    assert "above the rig cap 60" in capsys.readouterr().err


def test_release_swp_conventional_health_hook(wc_ws):
    (wc_ws / "apps" / "wc").mkdir(parents=True)
    (wc_ws / "apps" / "wc" / "health").write_text("#!/bin/sh\nexit 0\n")
    assert theia.cmd_release_swp(["wc", "--swp-version", "0.1.0"]) == 0
    app, members = _payload(wc_ws)
    assert app["health"] == "health" and "./health" in members


@pytest.mark.parametrize("bad", ["wait_s", "wait_s=x", "nope=3", "wait_s=0"])
def test_release_swp_rejects_bad_gate(wc_ws, bad, capsys):
    assert theia.cmd_release_swp(["wc", "--swp-version", "0.1.0", "--gate", bad]) == 2
    assert "bad --gate item" in capsys.readouterr().err


def test_release_swp_null_declares_no_processes(wc_ws):
    assert theia.cmd_release_swp(["wc", "--null", "--swp-version", "0.1.0"]) == 0
    app, _ = _payload(wc_ws)
    assert app["processes"] == []


def test_app_json_parses_with_the_rig_module(wc_ws, tmp_path):
    """The JSON release-swp writes is what the module's sed parser reads."""
    assert theia.cmd_release_swp(["wc", "--swp-version", "0.1.0",
                                  "--gate", "wait_s=180"]) == 0
    app, _ = _payload(wc_ws)
    f = tmp_path / "app.json"
    f.write_text(json.dumps(app, indent=2) + "\n")
    mod = (Path(__file__).resolve().parent.parent / "platform" / "runtime" / "ota"
           / "modules" / "theia-swp")
    import subprocess
    env = dict(os.environ, THEIA_SWP_SOURCE_ONLY="1")
    out = subprocess.run(
        ["sh", "-c", '. "$1"; _swp_processes "$2"; _gate_timing "" "$2" wait_s 60 300',
         "_", str(mod), str(f)], env=env, capture_output=True, text=True, check=True)
    assert out.stdout.split("\n")[:3] == ["ingest ingest", "link link", "180"]

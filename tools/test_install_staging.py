"""Unit tests for the local-install staging contract.

- ProcessLayer.resources are staged by `theia install` into
  releases/local/share/<fc>/data/<dest> — the SAME relative layout `theia dist`
  bakes into the deb at /opt/theia/share, so runtime::share_dir_self() resolves
  identically in the dev loop and on the device.
- deploy/config overrides expand ${THEIA_WORKSPACE}/${THEIA_ROOT} ONLY in the
  local install pass; `theia manifest` (which bakes into the committed, shipped
  dist/manifest) keeps them literal.
- `theia init`'s .mcp.json is relative + picks the framework venv in source mode.
"""
import json
import sys
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import theia  # noqa: E402  (tools/theia.py)


def _machine(tmp_path, resources):
    mdir = tmp_path / "dist" / "manifest" / "m"
    mdir.mkdir(parents=True)
    (mdir / "execution.json").write_text(json.dumps({"processes": [
        {"name": "percept", "resources": resources},
        {"name": "log", "resources": []},
    ]}))
    return mdir


def test_stage_resources_local_mirrors_share_layout(tmp_path):
    (tmp_path / "packs").mkdir()
    (tmp_path / "packs" / "a.lut").write_bytes(b"A")
    (tmp_path / "packs" / "b.lut").write_bytes(b"B")
    mdir = _machine(tmp_path, [
        {"src": "packs/a.lut"},                        # dest defaults to basename
        {"src": "packs/b.lut", "dest": "sub/b.lut"},
        {"src": "model:detector/yolo@1"},              # lazy → skipped
    ])
    rel = tmp_path / "install" / "m" / "releases" / "local"
    stale = rel / "share" / "gone" / "data" / "old.bin"
    stale.parent.mkdir(parents=True)
    stale.write_bytes(b"x")
    with mock.patch.object(theia, "WORKSPACE", tmp_path):
        assert theia._stage_resources_local(mdir, rel) == 0
    data = rel / "share" / "percept" / "data"
    assert (data / "a.lut").read_bytes() == b"A"
    assert (data / "sub" / "b.lut").read_bytes() == b"B"
    assert not stale.exists()                          # share/ rebuilt each install


def test_stage_resources_local_missing_src_fails(tmp_path):
    mdir = _machine(tmp_path, [{"src": "packs/missing.lut"}])
    with mock.patch.object(theia, "WORKSPACE", tmp_path):
        assert theia._stage_resources_local(mdir, tmp_path / "rel") == 1


def _override(tmp_path, body):
    ov = tmp_path / "deploy" / "config" / "m"
    ov.mkdir(parents=True)
    (ov / "wayplan.json").write_text(json.dumps(body))
    cfg = tmp_path / "cfg"
    cfg.mkdir()
    (cfg / "wayplan.json").write_text(json.dumps({"nodes": {"w": {"k": 1}}}))
    return cfg


def test_config_override_expands_anchors_only_on_install(tmp_path):
    body = {"nodes": {"w": {"log": "${THEIA_WORKSPACE}/out/s.jsonl",
                            "tags": ["${THEIA_ROOT}/x", 3]}}}
    cfg = _override(tmp_path, body)
    with mock.patch.object(theia, "WORKSPACE", tmp_path), \
         mock.patch.object(theia, "THEIA_ROOT", Path("/fw")):
        theia._apply_config_overrides("m", cfg)
        literal = json.loads((cfg / "wayplan.json").read_text())
        assert literal["nodes"]["w"]["log"] == "${THEIA_WORKSPACE}/out/s.jsonl"
        theia._apply_config_overrides("m", cfg, expand_anchors=True)
    got = json.loads((cfg / "wayplan.json").read_text())["nodes"]["w"]
    assert got == {"k": 1, "log": f"{tmp_path}/out/s.jsonl",
                   "tags": ["/fw/x", 3]}


def test_mcp_json_relative_and_uses_framework_venv(tmp_path):
    fw = tmp_path / "theia_ws"
    (fw / ".venv" / "bin").mkdir(parents=True)
    (fw / ".venv" / "bin" / "python").write_text("")
    ws = tmp_path / "chariot_ws"
    ws.mkdir()
    cfg = json.loads(theia._render_mcp_json(ws, fw))["mcpServers"]
    assert cfg["theia"]["command"] == "../theia_ws/.venv/bin/python"
    assert cfg["theia"]["args"] == ["../theia_ws/tools/theia_mcp.py"]
    assert cfg["work-with-me"]["args"] == [
        "../theia_ws/contrib/skills/work-with-me/server.py"]
    # deb mode (no framework venv) → the workspace's own venv
    (fw / ".venv" / "bin" / "python").unlink()
    cfg = json.loads(theia._render_mcp_json(ws, fw))["mcpServers"]
    assert cfg["artheia"]["command"] == ".venv/bin/python"

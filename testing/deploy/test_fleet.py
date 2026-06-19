"""fleet.py tests — the VUCM Mender Management-API client (deploy/vucm/fleet.py).

The reusable spine the real management server builds on. Hermetic: mock urllib so
no server is needed; assert the API path layout (oss v4 vs hosted v2), the multipart
upload framing, and the deploy/status calls. The live upload→deploy→pull→UCM loop
was proven against the dalek server; this guards the wire shape.
"""
import importlib.util
import sys
from pathlib import Path
from unittest import mock

import pytest

REPO = Path(__file__).resolve().parents[2]
FLEET = REPO / "deploy" / "vucm" / "fleet.py"


def _load():
    spec = importlib.util.spec_from_file_location("fleet", FLEET)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["fleet"] = mod  # register so mock.patch("fleet....") resolves
    spec.loader.exec_module(mod)
    return mod


fleet = _load()


def test_oss_vs_hosted_api_paths():
    """The OSS v4 server nests under /deployments/; hosted Mender flattens it."""
    oss = fleet.Mender("https://localhost", "tok", flavor="oss")
    hosted = fleet.Mender("https://hosted.mender.io", "tok", flavor="hosted")
    assert oss.art == "/api/management/v1/deployments/artifacts"
    assert oss.dep == "/api/management/v1/deployments/deployments"
    assert hosted.art == "/api/management/v2/deployments/artifacts"
    assert hosted.dep == "/api/management/v1/deployments"


def test_bearer_auth_header():
    m = fleet.Mender("https://h", "my-token")
    captured = {}

    def fake_urlopen(req, context=None):
        captured["auth"] = req.get_header("Authorization")
        return mock.MagicMock(status=200, read=lambda: b"[]",
                              headers={}, __enter__=lambda s: s,
                              __exit__=lambda *a: None)

    with mock.patch("fleet.urllib.request.urlopen", side_effect=fake_urlopen):
        m.list_artifacts()
    assert captured["auth"] == "Bearer my-token"


def test_upload_is_multipart_with_artifact_field(tmp_path):
    art = tmp_path / "2.0.0.mender"
    art.write_bytes(b"FAKE-MENDER-ARTIFACT")
    m = fleet.Mender("https://h", "tok")
    captured = {}

    def fake_urlopen(req, context=None):
        captured["ctype"] = req.get_header("Content-type")
        captured["body"] = req.data
        return mock.MagicMock(status=201, read=lambda: b"",
                              headers={"Location": "/artifacts/abc123"},
                              __enter__=lambda s: s, __exit__=lambda *a: None)

    with mock.patch("fleet.urllib.request.urlopen", side_effect=fake_urlopen):
        aid = m.upload_artifact(art, "desc")
    assert aid == "abc123"
    assert "multipart/form-data" in captured["ctype"]
    assert b'name="artifact"' in captured["body"]
    assert b"FAKE-MENDER-ARTIFACT" in captured["body"]


def test_create_deployment_posts_devices_and_artifact():
    m = fleet.Mender("https://h", "tok")
    captured = {}

    def fake_urlopen(req, context=None):
        import json
        captured["url"] = req.full_url
        captured["json"] = json.loads(req.data)
        return mock.MagicMock(status=201, read=lambda: b"",
                              headers={"Location": "/deployments/dep1"},
                              __enter__=lambda s: s, __exit__=lambda *a: None)

    with mock.patch("fleet.urllib.request.urlopen", side_effect=fake_urlopen):
        dep = m.create_deployment("roll", "2.0.0", ["dev1", "dev2"])
    assert dep == "dep1"
    assert captured["json"]["artifact_name"] == "2.0.0"
    assert captured["json"]["devices"] == ["dev1", "dev2"]
    assert captured["url"].endswith("/deployments/deployments")  # oss path


def test_cli_help_runs():
    import subprocess
    import sys
    r = subprocess.run([sys.executable, str(FLEET), "--help"],
                       capture_output=True, text=True)
    assert r.returncode == 0
    for cmd in ("upload", "deploy", "status", "release"):
        assert cmd in r.stdout

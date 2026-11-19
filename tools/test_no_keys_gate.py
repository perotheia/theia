"""Regression tests for the no-keys gate (hardening H: NO KEY MATERIAL IN ANY
SOFTWARE PACKAGE theia builds).

- tools/no_keys_gate.py: every archive type (dir, tar, tgz, zip, .deb (ar, gz and
  zstd members), .mender (tar of tars)) FAILS with a planted key and PASSES clean;
  the dev-CA certificate fails in release mode; --allow-dev-certs tolerates only
  the dev cert set under certs/; secrets (tskey-, AWS key id, MinIO password).
- theia.py wiring: `release-swp` on the services rig refuses a payload with a
  key, no longer ships the dev certs by default, refuses --allow-dev-certs with
  --s3; `release` pushes NOTHING when the manifest carries a key; `dist` removes
  a .deb that fails the gate.

Real keys/certs come from openssl + tools/gen_dev_certs.sh (the dev CA)."""
import io
import json
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path
from unittest import mock

import pytest

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

import no_keys_gate as nk  # noqa: E402
import theia  # noqa: E402

pytestmark = pytest.mark.skipif(shutil.which("openssl") is None,
                                reason="needs openssl to mint test keys")


# ── fixtures: real key material ─────────────────────────────────────────────

@pytest.fixture(scope="module")
def pki(tmp_path_factory):
    d = tmp_path_factory.mktemp("pki")
    dev = d / "dev"
    subprocess.run(["bash", str(TOOLS / "gen_dev_certs.sh"), str(dev), "localhost"],
                   check=True, capture_output=True)
    # A NON-dev certificate (a fleet-style CA) + its key.
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-subj", "/O=Theia Fleet/CN=fleet-ca", "-days", "1",
                    "-keyout", str(d / "fleet.key"), "-out", str(d / "fleet.crt")],
                   check=True, capture_output=True)
    return {"dev": dev, "key": (dev / "server.key").read_bytes(),
            "dev_crt": (dev / "server.crt").read_bytes(),
            "dev_ca": (dev / "ca.crt").read_bytes(),
            "fleet_crt": (d / "fleet.crt").read_bytes()}


def _tree(root: Path, files: dict) -> Path:
    for rel, data in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data if isinstance(data, bytes) else data.encode())
    return root


CLEAN = {"bin/com": b"\x7fELF" + b"\0" * 64, "config/com.json": '{"port": 7700}',
         "version.txt": "services-0.3.0\n"}


# ── archive builders ────────────────────────────────────────────────────────

def _tar(path: Path, files: dict, mode="w") -> Path:
    with tarfile.open(path, mode) as tf:
        for rel, data in files.items():
            data = data if isinstance(data, bytes) else data.encode()
            ti = tarfile.TarInfo("./" + rel)
            ti.size = len(data)
            tf.addfile(ti, io.BytesIO(data))
    return path


def _zip(path: Path, files: dict) -> Path:
    with zipfile.ZipFile(path, "w") as zf:
        for rel, data in files.items():
            zf.writestr(rel, data)
    return path


def _ar(path: Path, members: "list[tuple[str, bytes]]") -> Path:
    out = bytearray(b"!<arch>\n")
    for name, data in members:
        hdr = (f"{name + '/':<16}{0:<12}{0:<6}{0:<6}{'100644':<8}{len(data):<10}`\n")
        out += hdr.encode() + data + (b"\n" if len(data) % 2 else b"")
    path.write_bytes(bytes(out))
    return path


def _deb(path: Path, files: dict) -> Path:
    """A .deb exactly as rules/deb.bzl builds it (ar of gz tars)."""
    data = _tar(path.with_suffix(".data.tgz"), {"opt/theia/" + k: v for k, v in files.items()}, "w:gz")
    ctrl = _tar(path.with_suffix(".ctrl.tgz"), {"control": "Package: t\nVersion: 1\n"}, "w:gz")
    return _ar(path, [("debian-binary", b"2.0\n"), ("control.tar.gz", ctrl.read_bytes()),
                      ("data.tar.gz", data.read_bytes())])


def _mender(path: Path, files: dict) -> Path:
    """A .mender: a tar of version + manifest + header.tar.gz + data/0000.tar.gz,
    the payload being the SWP tarball (itself a .tar.gz) — tar of tars of tars."""
    payload = _tar(path.with_suffix(".payload.tgz"), files, "w:gz")
    data0 = _tar(path.with_suffix(".data0.tgz"),
                 {"services-0.3.0.tar.gz": payload.read_bytes(),
                  "version.txt": "services-0.3.0\n"}, "w:gz")
    header = _tar(path.with_suffix(".hdr.tgz"),
                  {"header-info": "{}", "headers/0000/type-info": '{"type":"theia-swp"}'},
                  "w:gz")
    return _tar(path, {"version": '{"format":"mender","version":3}',
                       "manifest": "", "header.tar.gz": header.read_bytes(),
                       "data/0000.tar.gz": data0.read_bytes()})


def _dpkg_deb_zstd(tmp: Path, files: dict) -> Path:
    root = _tree(tmp / "root", {"opt/theia/" + k: v for k, v in files.items()})
    (root / "DEBIAN").mkdir()
    (root / "DEBIAN" / "control").write_text(
        "Package: t\nVersion: 1\nArchitecture: all\nMaintainer: t <t@t>\nDescription: t\n")
    out = tmp / "t_1_all.deb"
    subprocess.run(["dpkg-deb", "-Zzstd", "--root-owner-group", "--build",
                    str(root), str(out)], check=True, capture_output=True)
    return out


BUILDERS = {
    "dir": lambda tmp, files: _tree(tmp / "tree", files),
    "tar": lambda tmp, files: _tar(tmp / "a.tar", files),
    "tgz": lambda tmp, files: _tar(tmp / "a.tar.gz", files, "w:gz"),
    "txz": lambda tmp, files: _tar(tmp / "a.tar.xz", files, "w:xz"),
    "zip": lambda tmp, files: _zip(tmp / "a.zip", files),
    "deb": lambda tmp, files: _deb(tmp / "a_1_amd64.deb", files),
    "mender": lambda tmp, files: _mender(tmp / "a.mender", files),
}


def _reasons(findings):
    return {f.reason for f in findings}


# ── the scanner ─────────────────────────────────────────────────────────────

@pytest.mark.parametrize("kind", sorted(BUILDERS))
def test_clean_artifact_passes(kind, tmp_path):
    art = BUILDERS[kind](tmp_path, CLEAN)
    assert nk.scan(art) == []
    assert nk.gate([art]) is True


@pytest.mark.parametrize("kind", sorted(BUILDERS))
def test_planted_key_file_fails(kind, tmp_path, pki, capsys):
    art = BUILDERS[kind](tmp_path, {**CLEAN, "psp/leak.key": pki["key"]})
    found = nk.scan(art)
    assert "private-key file name (.key)" in _reasons(found)
    assert "private key (PEM)" in _reasons(found)
    assert all(f.where().endswith("psp/leak.key") for f in found)
    assert nk.gate([art]) is False
    err = capsys.readouterr().err
    assert "NO-KEYS GATE" in err and "leak.key" in err and str(art) in err


@pytest.mark.parametrize("kind", sorted(BUILDERS))
def test_key_pasted_into_any_file_fails(kind, tmp_path, pki):
    # a key inside a JSON config (JSON-escaped newlines) under an innocent name
    pasted = json.dumps({"tls_key": pki["key"].decode()})
    art = BUILDERS[kind](tmp_path, {**CLEAN, "config/com.json": pasted})
    assert _reasons(nk.scan(art)) == {"private key (PEM)"}


@pytest.mark.parametrize("kind", sorted(BUILDERS))
def test_dev_ca_certificate_fails_in_release(kind, tmp_path, pki):
    art = BUILDERS[kind](tmp_path, {**CLEAN, "config/ca.crt": pki["dev_ca"]})
    assert _reasons(nk.scan(art)) == {nk.DEV_CERT}
    assert nk.gate([art]) is False


def test_dev_ca_detected_in_der_too(tmp_path, pki):
    der = subprocess.run(["openssl", "x509", "-outform", "DER"], input=pki["dev_crt"],
                         capture_output=True, check=True).stdout
    art = _tree(tmp_path / "t", {"x.der": der})
    assert _reasons(nk.scan(art)) == {nk.DEV_CERT}


def test_fleet_certificate_passes(tmp_path, pki):
    art = _tar(tmp_path / "a.tgz", {**CLEAN, "pki/ca.crt": pki["fleet_crt"],
                                    "pki/ca.pem": pki["fleet_crt"]}, "w:gz")
    assert nk.scan(art) == []


def test_pem_naming_private_key_fails(tmp_path):
    art = _tree(tmp_path / "t", {"x.pem": "# PRIVATE KEY goes here\n"})
    assert _reasons(nk.scan(art)) == {"*.pem containing 'PRIVATE KEY'"}


def test_p12_name_fails(tmp_path):
    assert nk.scan(_tree(tmp_path / "t", {"id.p12": b"\x30\x82"}))


def test_deb_with_zstd_members(tmp_path, pki):
    if not shutil.which("dpkg-deb"):
        pytest.skip("needs dpkg-deb")
    good = _dpkg_deb_zstd(tmp_path / "g", CLEAN)
    assert nk.scan(good) == []
    bad = _dpkg_deb_zstd(tmp_path / "b", {**CLEAN, "certs/server.key": pki["key"]})
    found = nk.scan(bad)
    assert found and all("data.tar.zst!" in f.where() for f in found)


def test_mender_reports_the_nested_path(tmp_path, pki):
    art = _mender(tmp_path / "s.mender", {**CLEAN, "manifest/master/certs/server.key": pki["key"]})
    where = {f.where() for f in nk.scan(art)}
    assert any(w.endswith("data/0000.tar.gz!./services-0.3.0.tar.gz!"
                          "./manifest/master/certs/server.key") for w in where)


@pytest.mark.parametrize("text", [
    "TS_AUTHKEY=tskey-auth-kDEADBEEF11CNTRL-abcdefghijklmnop\n",
    "aws_access_key_id = AKIAIOSFODNN7EXAMPLE\n",
    "[default]\naws_secret_access_key = wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY\n",
    "MINIO_ROOT_PASSWORD=s3cr3t-passw0rd\n",
    "export AWS_SECRET_ACCESS_KEY='abcdEFGH1234/xyz'\n",
])
def test_secrets_fail(tmp_path, text):
    assert nk.scan(_tree(tmp_path / "t", {"deploy/env": text}))


@pytest.mark.parametrize("text", [
    'pw = os.environ.get("MINIO_PASSWORD", "theiaminio")\n',      # theia.py's read
    'AWS_SECRET_ACCESS_KEY="${MINIO_PASSWORD:-theiaminio}"\n',     # shell expansion
    "AWS_SECRET_ACCESS_KEY=$MINIO_PASSWORD\n",
    '_SK_START = b"-----BEGIN OPENSSH PRIVATE KEY-----"\n',        # a parser, no body
])
def test_non_secrets_pass(tmp_path, text):
    assert nk.scan(_tree(tmp_path / "t", {"x.py": text})) == []


def test_allow_dev_certs_tolerates_only_the_dev_set_in_certs(tmp_path, pki, capsys):
    devset = {f"manifest/master/certs/{f.name}": f.read_bytes()
              for f in sorted(pki["dev"].iterdir()) if f.suffix in (".crt", ".key")}
    art = _tar(tmp_path / "dev.tgz", {**CLEAN, **devset}, "w:gz")
    assert nk.gate([art]) is False                       # release mode
    assert nk.gate([art], allow_dev_certs=True) is True  # local dev bundle
    assert "allowed by --allow-dev-certs" in capsys.readouterr().err
    # a key OUTSIDE certs/, or a secret, still fails with the flag
    for extra in ({"psp/leak.key": pki["key"]},
                  {"deploy/env": "MINIO_ROOT_PASSWORD=s3cr3t-passw0rd\n"}):
        art2 = _tar(tmp_path / "dev2.tgz", {**CLEAN, **devset, **extra}, "w:gz")
        assert nk.gate([art2], allow_dev_certs=True) is False


def test_uninspectable_member_fails_closed(tmp_path):
    art = _tar(tmp_path / "a.tar", {"data.tar.gz": b"\x1f\x8b\x08\x00garbage"})
    found = nk.scan(art)
    assert found and found[0].kind == "uninspectable"
    assert nk.gate([art], allow_dev_certs=True) is False


def test_missing_artifact_fails(tmp_path):
    assert nk.gate([tmp_path / "nope.deb"]) is False


def test_the_shipped_tools_scan_clean():
    """The scanner ships in the theia-framework deb next to theia.py — neither may
    trip the gate it implements."""
    assert nk.scan(TOOLS / "no_keys_gate.py") == []
    assert nk.scan(TOOLS / "theia.py") == []


def test_cli_exit_codes(tmp_path, pki):
    good = _tree(tmp_path / "g", CLEAN)
    bad = _tree(tmp_path / "b", {"x.key": pki["key"]})
    run = lambda *a: subprocess.run([sys.executable, str(TOOLS / "no_keys_gate.py"), *a],
                                    capture_output=True, text=True)
    assert run(str(good)).returncode == 0
    r = run("--label", "pkg_1_amd64.deb", str(bad))
    assert r.returncode == 1 and "pkg_1_amd64.deb!x.key" in r.stderr
    assert run().returncode == 2


# ── theia.py: release-swp on the services rig ───────────────────────────────

def _services_rig(ws: Path, pki) -> Path:
    """dist/manifest as `theia manifest services` lays it: master + zonal, the
    `services` application, and the dev cert set staged per machine."""
    mdir = ws / "dist" / "manifest"
    (mdir).mkdir(parents=True)
    (mdir / "machines.json").write_text(json.dumps({
        "machines": ["master", "zonal"], "rig": "services", "apps": [],
        "role_map": {"master": "master", "zonal": "zonal"}}))
    for m, procs in (("master", ["com", "per", "sm", "ucm"]), ("zonal", ["ucm", "shwa"])):
        d = mdir / m
        (d / "config").mkdir(parents=True)
        (d / "machine.json").write_text(json.dumps({"name": m, "arch": "x86_64", "role": m}))
        (d / "application.json").write_text(json.dumps({"applications": [
            {"name": "services", "host_machine": m, "processes": procs}]}))
        (d / "executor.json").write_text(json.dumps({"name": "root", "children": [
            {"name": p, "start_cmd": f"bin/{p}"} for p in procs]}))
        (d / "execution.json").write_text(json.dumps({"processes": []}))
        (d / "config" / "com.json").write_text("{}")
        shutil.copytree(pki["dev"], d / "certs",
                        ignore=shutil.ignore_patterns("*.csr", "*.ext", "*.srl"))
    return mdir


def _fake_run(argv, cwd=None):
    """bazel → ok; mender-artifact → a .mender (tar of tars) from the --file parts."""
    if argv[0] == "bazel":
        return 0
    if argv[0] == "mender-artifact":
        files = [argv[i + 1] for i, a in enumerate(argv) if a == "--file"]
        out = Path(argv[argv.index("--output-path") + 1])
        data0 = io.BytesIO()
        with tarfile.open(fileobj=data0, mode="w:gz") as tf:
            for f in files:
                tf.add(f, arcname=Path(f).name)
        _tar(out, {"version": "{}", "header.tar.gz": b"", "data/0000.tar.gz": data0.getvalue()})
        return 0
    raise AssertionError(f"unexpected command {argv}")


@pytest.fixture
def services_ws(tmp_path, pki, monkeypatch):
    _services_rig(tmp_path, pki)
    monkeypatch.setattr(theia, "WORKSPACE", tmp_path)
    monkeypatch.setattr(theia, "_run", _fake_run)
    monkeypatch.setattr(theia, "_mender_artifact_bin", lambda: "mender-artifact")
    monkeypatch.setattr(theia, "SWP_SIGN_KEY", tmp_path / "no-sign.key")
    monkeypatch.delenv("THEIA_SWP_SIGN_KEY", raising=False)
    return tmp_path


def _names(tgz: Path) -> "list[str]":
    with tarfile.open(tgz) as tf:
        return tf.getnames()


SWP = ["services", "--null", "--swp-version", "0.3.1"]


def test_release_swp_services_rig_ships_no_dev_certs(services_ws, capsys):
    assert theia.cmd_release_swp(SWP) == 0
    out = services_ws / "dist" / "apps" / "services"
    tgz, mender = out / "services-0.3.1-amd64.tar.gz", out / "services-0.3.1-amd64.mender"
    assert tgz.is_file() and mender.is_file()
    assert not [n for n in _names(tgz) if "certs" in n]
    assert nk.scan(tgz) == [] and nk.scan(mender) == []
    err = capsys.readouterr().err
    assert "not shipping dist/manifest/master/certs" in err
    assert "no-keys gate OK" in err


def test_release_swp_refuses_a_key_in_the_payload(services_ws, pki, capsys):
    leak = services_ws / "leak"
    leak.mkdir()
    (leak / "server.key").write_bytes(pki["key"])
    rc = theia.cmd_release_swp(SWP + ["--asset", f"{leak}:psp"])
    assert rc == 1
    err = capsys.readouterr().err
    assert "dist/apps/services/services-0.3.1-amd64.tar.gz!./psp/leak/server.key" in err
    assert "REFUSED" in err
    out = services_ws / "dist" / "apps" / "services"
    assert not (out / "services-0.3.1-amd64.tar.gz").exists()      # removed, not pushable
    assert not (out / "services-0.3.1-amd64.mender").exists()      # never packed


def test_release_swp_refuses_a_dev_ca_cert_in_the_payload(services_ws, pki, capsys):
    a = services_ws / "a"
    a.mkdir()
    (a / "trust.crt").write_bytes(pki["dev_ca"])
    assert theia.cmd_release_swp(SWP + ["--asset", f"{a}:psp"]) == 1
    assert "O=Theia Dev" in capsys.readouterr().err


def test_release_swp_refuses_allow_dev_certs_with_s3(services_ws, capsys):
    rc = theia.cmd_release_swp(SWP + ["--allow-dev-certs", "--s3", "http://x:9000"])
    assert rc == 2
    assert "refused with --s3" in capsys.readouterr().err
    assert not (services_ws / "dist" / "apps").exists()


def test_release_swp_allow_dev_certs_local_bundle(services_ws):
    assert theia.cmd_release_swp(SWP + ["--allow-dev-certs"]) == 0
    tgz = services_ws / "dist" / "apps" / "services" / "services-0.3.1-amd64.tar.gz"
    assert "./manifest/master/certs/server.key" in _names(tgz)
    assert nk.gate([tgz]) is False                 # never publishable as is


def test_release_swp_deb_refuses_a_key(services_ws, pki, capsys):
    if not shutil.which("dpkg-deb"):
        pytest.skip("needs dpkg-deb")
    leak = services_ws / "l.pem"
    leak.write_bytes(pki["key"])
    assert theia.cmd_release_swp(SWP + ["--deb", "--asset", f"{leak}:psp"]) == 1
    assert not list((services_ws / "dist" / "apps" / "services").glob("*.deb"))


# ── theia.py: release (runtime plane) and dist ──────────────────────────────

def test_release_pushes_nothing_when_the_manifest_holds_a_key(services_ws, pki, capsys):
    deb_dir = services_ws / "dist" / "debian" / "theia-runtime"
    deb_dir.mkdir(parents=True)
    _deb(deb_dir / "theia-runtime_0.3.0_amd64.deb", CLEAN)
    # the dev certs/ are excluded from the upload copy; a key elsewhere is not
    (services_ws / "dist" / "manifest" / "master" / "config" / "tls.pem").write_bytes(pki["key"])
    calls = []
    with mock.patch.object(theia, "cmd_dist", return_value=0), \
         mock.patch.object(theia.shutil, "which", return_value="/usr/bin/aws"), \
         mock.patch("subprocess.run", side_effect=lambda a, **k: calls.append(a)):
        rc = theia.cmd_release(["services", "--s3", "http://x:9000", "--version", "0.3.0"])
    assert rc == 1
    assert calls == []                               # not even `aws mb`
    err = capsys.readouterr().err
    assert "master/config/tls.pem" in err and "NOTHING pushed" in err


def test_release_upload_copy_excludes_dev_certs(services_ws, capsys):
    deb_dir = services_ws / "dist" / "debian" / "theia-runtime"
    deb_dir.mkdir(parents=True)
    _deb(deb_dir / "theia-runtime_0.3.0_amd64.deb", CLEAN)
    calls = []

    def _sp(a, **k):
        calls.append(a)
        return mock.Mock(returncode=0)
    with mock.patch.object(theia, "cmd_dist", return_value=0), \
         mock.patch.object(theia.shutil, "which", return_value="/usr/bin/aws"), \
         mock.patch("subprocess.run", side_effect=_sp):
        rc = theia.cmd_release(["services", "--s3", "http://x:9000", "--version", "0.3.0"])
    assert rc == 0
    up = services_ws / "dist" / "debian" / "_manifest-upload"
    assert (up / "master" / "executor.json").is_file()
    assert not list(up.rglob("certs")) and not list(up.rglob("*.key"))
    tgz = services_ws / "dist" / "debian" / "manifest-0.3.0-amd64.tar.gz"
    assert nk.scan(tgz) == []
    assert any("sync" in c and str(up) in c for c in calls)


def test_release_refuses_allow_dev_certs_with_s3(services_ws, capsys):
    with mock.patch.object(theia, "cmd_dist") as dist:
        rc = theia.cmd_release(["services", "--s3", "http://x:9000", "--allow-dev-certs"])
    assert rc == 2 and not dist.called


def test_dist_app_removes_a_deb_that_fails_the_gate(tmp_path, pki, monkeypatch):
    mdir = tmp_path / "dist" / "manifest"
    (mdir / "central").mkdir(parents=True)
    (mdir / "machines.json").write_text(json.dumps({"machines": ["central"], "apps": ["a"]}))
    (mdir / "central" / "machine.json").write_text(json.dumps({"arch": "x86_64"}))
    built = tmp_path / "bazel-bin" / "dist" / "manifest" / "central.deb"
    built.parent.mkdir(parents=True)
    _deb(built, {**CLEAN, "share/percept/data/id.key": pki["key"]})   # a "resource"
    monkeypatch.setattr(theia, "WORKSPACE", tmp_path)
    monkeypatch.setattr(theia, "_run", lambda *a, **k: 0)
    monkeypatch.setattr(theia, "_inject_resources", lambda *a, **k: 0)
    assert theia.cmd_dist(["single"]) == 1
    assert not (mdir / "central" / "central.deb").exists()

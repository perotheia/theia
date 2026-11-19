#!/usr/bin/env python3
"""no_keys_gate — NO KEY MATERIAL IN ANY SOFTWARE PACKAGE theia builds (hardening H).

ONE scanner, used by every packaging path (`theia release`, `release-swp`,
`release-role`, `dist`, `build-debs`, `models push`, the bazel `pkg_deb` rule and
the release-deb CI workflow). It walks a directory tree or an archive and reports
every piece of key material it finds; the callers turn any finding into a FAILED
build.

Archives are recognised by their bytes, not their names, and opened recursively:
tar (plain / gz / xz / bz2 / zstd), zip (and .whl), ar (.deb / .ipk: the
data.tar.* + control.tar.* inside), .mender (a tar of tars: header.tar.gz +
data/0000.tar.gz), and a single compressed file. A member that cannot be opened
(zstd with no decompressor) is a finding too: the gate fails closed.

It FAILS on
  * files named *.key, *.p12, *.pfx (private-key containers, whatever the content)
  * any PEM private key: -----BEGIN [RSA |EC |DSA |ENCRYPTED |OPENSSH |PGP ]PRIVATE KEY-----
    (so a *.pem holding a key, a key pasted into a config file, a key compiled
    into a binary)
  * a certificate whose subject or issuer is O=Theia Dev (the dev CA of
    tools/gen_dev_certs.sh): the dev cert set never ships in a release
  * Tailscale auth keys (tskey-...), AWS access key ids (AKIA/ASIA...), and secret
    assignments: aws_secret_access_key = ..., AWS_SECRET_ACCESS_KEY= /
    MINIO_PASSWORD= / MINIO_ROOT_PASSWORD= / MINIO_SECRET_KEY= with a literal value
    (the MinIO/S3 credentials the tools read from the environment)

--allow-dev-certs (a LOCAL dev bundle only; the callers refuse it with --s3)
tolerates the dev cert set: dev-CA certificates and private keys that sit in a
`certs/` directory. Every other finding still fails.

CLI:  python3 tools/no_keys_gate.py [--allow-dev-certs] [--label NAME] PATH...
      exit 0 = clean, 1 = key material found, 2 = usage.
Stdlib only (theia.py is pure stdlib; this runs before any venv exists).
"""
from __future__ import annotations

import base64
import bz2
import io
import lzma
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
import gzip
from dataclasses import dataclass
from pathlib import Path
from typing import IO, Iterable

# ── What counts as key material ─────────────────────────────────────────────
# NB: written so this file never matches itself (every header is split by a
# group), because the scanner ships inside the theia-framework deb it gates.
_KEY_NAME_SUFFIXES = (".key", ".p12", ".pfx")

_CONTENT_PATTERNS: "list[tuple[str, re.Pattern[bytes]]]" = [
    # The header AND a key body (PEM headers like Proc-Type/DEK-Info allowed, a
    # JSON/YAML-escaped "\n" too): a parser that only names the header (e.g. the
    # `cryptography` wheel's ssh.py) is not key material; a pasted key is.
    ("private key (PEM)", re.compile(
        rb"-----BEGIN (?:RSA |EC |DSA |ENCRYPTED |OPENSSH |PGP )?PRIVATE KEY(?: BLOCK)?-----"
        rb"(?:\s|\\[nr])*(?:[A-Za-z-]+: ?[^\r\n\\]*(?:\s|\\[nr])+)*[A-Za-z0-9+/=]{40}")),
    ("Tailscale auth key (tskey-)", re.compile(
        rb"tskey-(?:auth|client|api|scim|webhook)?-?[A-Za-z0-9][A-Za-z0-9-]{9,}")),
    ("AWS access key id", re.compile(rb"\b(?:AKIA|ASIA)[0-9A-Z]{16}\b")),
    ("S3 secret (aws_secret_access_key = ...)", re.compile(
        rb"(?i)\baws_secret_access_key\s*[=:]\s*[\"']?[A-Za-z0-9/+=]{16,}")),
    ("S3/MinIO secret assignment", re.compile(
        rb"\b(?:AWS_SECRET_ACCESS_KEY|MINIO_(?:ROOT_)?PASSWORD|MINIO_SECRET_KEY)[\"']?"
        rb"\s*[=:]\s*[\"']?[A-Za-z0-9/+=_][A-Za-z0-9/+=_\-]{7,}")),
]

# A certificate naming the dev CA: the DER of the attribute O=Theia Dev
# (OID 2.5.4.10, then a UTF8/Printable/T61/IA5/BMP string of length 9).
_DEV_CA_DER = re.compile(rb"\x06\x03\x55\x04\x0a[\x0c\x13\x14\x16]\x09Theia Dev")
_PEM_CERT = re.compile(
    rb"-----BEGIN CERTIFICATE-----([A-Za-z0-9+/=\s]+?)-----END CERTIFICATE-----")

PRIVATE_KEY = "private key"
DEV_CERT = "dev-CA certificate (O=Theia Dev)"

_CHUNK = 4 << 20          # content is scanned in 4 MiB windows ...
_OVERLAP = 16 << 10       # ... overlapping by 16 KiB (a PEM cert/key fits)
_SPOOL = 32 << 20         # archive members up to 32 MiB stay in memory
_MAX_DEPTH = 6            # nested archives deeper than this are a finding


@dataclass(frozen=True)
class Finding:
    artifact: str          # the top-level path (or --label) that was scanned
    member: str            # where inside it ("a.deb!data.tar.zst!./opt/x.key"); "" = itself
    reason: str
    kind: str              # PRIVATE_KEY, DEV_CERT, or "secret" / "uninspectable"

    def where(self) -> str:
        return f"{self.artifact}!{self.member}" if self.member else self.artifact

    def in_certs_dir(self) -> bool:
        last = self.member.rsplit("!", 1)[-1] if self.member else self.artifact
        return "certs" in Path(last).parts[:-1]

    def dev_cert_set(self) -> bool:
        """True for what --allow-dev-certs tolerates: the dev cert set (dev-CA
        certificates, private keys) inside a certs/ directory."""
        return self.kind in (PRIVATE_KEY, DEV_CERT) and self.in_certs_dir()


# ── Content scan ────────────────────────────────────────────────────────────

def _scan_bytes(buf: bytes, hits: "dict[str, str]") -> None:
    for reason, rx in _CONTENT_PATTERNS:
        if reason not in hits and rx.search(buf):
            hits[reason] = "secret" if not reason.startswith("private key") else PRIVATE_KEY
    if DEV_CERT not in hits:
        if _DEV_CA_DER.search(buf):
            hits[DEV_CERT] = DEV_CERT
            return
        for m in _PEM_CERT.finditer(buf):
            try:
                der = base64.b64decode(re.sub(rb"\s+", b"", m.group(1)))
            except (ValueError, TypeError):
                continue
            if _DEV_CA_DER.search(der):
                hits[DEV_CERT] = DEV_CERT
                return


def _scan_stream(f: IO[bytes]) -> "dict[str, str]":
    hits: "dict[str, str]" = {}
    tail = b""
    while True:
        chunk = f.read(_CHUNK)
        if not chunk:
            break
        buf = tail + chunk
        _scan_bytes(buf, hits)
        tail = buf[-_OVERLAP:]
    return hits


# ── Archive sniffing ────────────────────────────────────────────────────────

def _sniff(head: bytes) -> "str | None":
    if head.startswith(b"!<arch>\n"):
        return "ar"
    if head.startswith(b"PK\x03\x04") or head.startswith(b"PK\x05\x06"):
        return "zip"
    if head.startswith(b"\x1f\x8b"):
        return "gz"
    if head.startswith(b"\xfd7zXZ\x00"):
        return "xz"
    if head.startswith(b"BZh"):
        return "bz2"
    if head.startswith(b"\x28\xb5\x2f\xfd"):
        return "zstd"
    if len(head) >= 262 and head[257:262] == b"ustar":
        return "tar"
    return None


def _decompress_zstd(src: IO[bytes], dst: IO[bytes]) -> bool:
    try:                                   # Python 3.14+
        from compression import zstd as _z  # type: ignore
        dst.write(_z.decompress(src.read()))
        return True
    except ImportError:
        pass
    try:
        import zstandard as _zs  # type: ignore
        _zs.ZstdDecompressor().copy_stream(src, dst)
        return True
    except ImportError:
        pass
    exe = shutil.which("zstd")
    if not exe:
        return False
    with tempfile.TemporaryFile() as tin:
        shutil.copyfileobj(src, tin)
        tin.seek(0)
        r = subprocess.run([exe, "-dc"], stdin=tin, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL)
    if r.returncode != 0:
        return False
    dst.write(r.stdout)
    return True


class _Scanner:
    def __init__(self, artifact: str) -> None:
        self.artifact = artifact
        self.findings: "list[Finding]" = []

    def _add(self, member: str, reason: str, kind: str) -> None:
        self.findings.append(Finding(self.artifact, member, reason, kind))

    @staticmethod
    def _join(parent: str, name: str) -> str:
        return f"{parent}!{name}" if parent else name

    def name_check(self, member: str, name: str) -> None:
        if name.lower().endswith(_KEY_NAME_SUFFIXES):
            self._add(member, f"private-key file name ({Path(name).suffix})", PRIVATE_KEY)

    def pem_check(self, member: str, name: str, f: IO[bytes]) -> None:
        """A *.pem that mentions a PRIVATE KEY at all (even a bare header)."""
        if not name.lower().endswith(".pem"):
            return
        data = f.read(_CHUNK)
        f.seek(0)
        if b"PRIVATE KEY" in data:
            self._add(member, "*.pem containing 'PRIVATE KEY'", PRIVATE_KEY)

    # A seekable binary stream positioned at 0.
    def blob(self, member: str, f: IO[bytes], depth: int) -> None:
        head = f.read(512)
        f.seek(0)
        kind = _sniff(head)
        if kind is None:
            for reason, k in _scan_stream(f).items():
                self._add(member, reason, k)
            return
        if depth >= _MAX_DEPTH:
            self._add(member, f"nested {kind} archive deeper than {_MAX_DEPTH} levels "
                      "(not inspected)", "uninspectable")
            return
        getattr(self, f"_{kind}")(member, f, depth + 1)

    def _spool_from(self, src: IO[bytes]) -> "tempfile.SpooledTemporaryFile[bytes]":
        sp = tempfile.SpooledTemporaryFile(max_size=_SPOOL)
        shutil.copyfileobj(src, sp)
        sp.seek(0)
        return sp

    def _decompressed(self, member: str, f: IO[bytes], depth: int, opener) -> None:
        try:
            with opener(f) as d, self._spool_from(d) as sp:
                self.blob(member, sp, depth)
        except (OSError, EOFError, lzma.LZMAError) as e:
            self._add(member, f"corrupt compressed data ({e})", "uninspectable")

    def _gz(self, member, f, depth):
        self._decompressed(member, f, depth,
                           lambda x: gzip.GzipFile(fileobj=x, mode="rb"))

    def _xz(self, member, f, depth):
        self._decompressed(member, f, depth, lambda x: lzma.LZMAFile(x, mode="rb"))

    def _bz2(self, member, f, depth):
        self._decompressed(member, f, depth, lambda x: bz2.BZ2File(x, mode="rb"))

    def _zstd(self, member, f, depth):
        with tempfile.SpooledTemporaryFile(max_size=_SPOOL) as sp:
            if not _decompress_zstd(f, sp):
                self._add(member, "zstd-compressed member and no zstd decompressor "
                          "(install zstd) — cannot prove it holds no keys",
                          "uninspectable")
                return
            sp.seek(0)
            self.blob(member, sp, depth)

    def _tar(self, member, f, depth):
        try:
            tf = tarfile.open(fileobj=f, mode="r:")
        except tarfile.TarError as e:
            self._add(member, f"unreadable tar ({e})", "uninspectable")
            return
        with tf:
            for ti in tf:
                if ti.isdir():
                    continue
                m = self._join(member, ti.name)
                self.name_check(m, ti.name)
                if not ti.isreg():
                    continue
                src = tf.extractfile(ti)
                if src is None:
                    continue
                with self._spool_from(src) as sp:
                    self.pem_check(m, ti.name, sp)
                    self.blob(m, sp, depth)

    def _zip(self, member, f, depth):
        try:
            zf = zipfile.ZipFile(f)
        except zipfile.BadZipFile as e:
            self._add(member, f"unreadable zip ({e})", "uninspectable")
            return
        with zf:
            for zi in zf.infolist():
                if zi.is_dir():
                    continue
                m = self._join(member, zi.filename)
                self.name_check(m, zi.filename)
                with zf.open(zi) as src, self._spool_from(src) as sp:
                    self.pem_check(m, zi.filename, sp)
                    self.blob(m, sp, depth)

    def _ar(self, member, f, depth):
        f.seek(8)
        while True:
            hdr = f.read(60)
            if len(hdr) < 60:
                break
            name = hdr[0:16].decode("ascii", "replace").strip()
            if name.endswith("/") and name not in ("/", "//"):
                name = name[:-1]
            try:
                size = int(hdr[48:58].decode("ascii").strip() or "0")
            except ValueError:
                self._add(member, "corrupt ar header", "uninspectable")
                return
            data = f.read(size)
            if size % 2:
                f.read(1)
            if name in ("/", "//", "__.SYMDEF"):
                continue
            m = self._join(member, name)
            self.name_check(m, name)
            self.blob(m, io.BytesIO(data), depth)

    def path(self, p: Path, member: str = "") -> None:
        if p.is_symlink():
            return
        if p.is_dir():
            for root, dirs, files in os.walk(p, followlinks=False):
                dirs.sort()
                for fn in sorted(files):
                    fp = Path(root) / fn
                    if fp.is_symlink() or not fp.is_file():
                        continue
                    rel = fp.relative_to(p).as_posix()
                    m = self._join(member, rel)
                    self.name_check(m, fn)
                    with open(fp, "rb") as fh:
                        self.pem_check(m, fn, fh)
                        self.blob(m, fh, 0)
            return
        self.name_check(member, p.name)
        with open(p, "rb") as fh:
            self.pem_check(member, p.name, fh)
            self.blob(member, fh, 0)


def scan(path: "str | os.PathLike", label: "str | None" = None) -> "list[Finding]":
    """Every piece of key material in `path` (a directory or a file/archive)."""
    p = Path(path)
    sc = _Scanner(label or str(path))
    if not p.exists():
        sc._add("", "artifact does not exist", "uninspectable")
        return sc.findings
    sc.path(p)
    return sc.findings


def gate(paths: "Iterable[str | os.PathLike]", *, verb: str = "theia",
         allow_dev_certs: bool = False, labels: "dict | None" = None,
         out: "IO[str] | None" = None) -> bool:
    """Scan `paths`; print every finding as `<verb>: NO-KEYS GATE: <where>: <reason>`.
    Returns True when the build may proceed (nothing found, or only the dev cert
    set under --allow-dev-certs). The caller fails the build on False."""
    out = out or sys.stderr
    blocking: "list[Finding]" = []
    tolerated: "list[Finding]" = []
    n = 0
    for p in paths:
        n += 1
        for fd in scan(p, (labels or {}).get(str(p))):
            (tolerated if allow_dev_certs and fd.dev_cert_set() else blocking).append(fd)
    for fd in tolerated:
        print(f"{verb}: NO-KEYS GATE: allowed by --allow-dev-certs (LOCAL dev bundle, "
              f"never publish): {fd.where()}: {fd.reason}", file=out)
    if blocking:
        for fd in blocking:
            print(f"{verb}: NO-KEYS GATE: {fd.where()}: {fd.reason}", file=out)
        print(f"{verb}: NO-KEYS GATE FAILED — {len(blocking)} finding(s) of key material "
              "in the artifact(s) above. A rig's key lives in the fleet PKI "
              "(/etc/theia/pki, generated on the rig), never in a package. Remove it "
              "from the payload"
              + ("" if allow_dev_certs else
                 "; for a LOCAL dev bundle that must carry the dev cert set, "
                 "--allow-dev-certs (refused with --s3)") + ".", file=out)
        return False
    print(f"{verb}: no-keys gate OK ({n} artifact(s)"
          + (f", {len(tolerated)} dev-cert item(s) allowed" if tolerated else "") + ")",
          file=out)
    return True


def main(argv: "list[str] | None" = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    allow = "--allow-dev-certs" in args
    label = None
    paths: "list[str]" = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--allow-dev-certs":
            pass
        elif a == "--label" and i + 1 < len(args):
            label = args[i + 1]
            i += 1
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        elif a.startswith("-"):
            print(f"no_keys_gate: unknown option {a}", file=sys.stderr)
            return 2
        else:
            paths.append(a)
        i += 1
    if not paths:
        print("usage: no_keys_gate.py [--allow-dev-certs] [--label NAME] PATH...",
              file=sys.stderr)
        return 2
    labels = {paths[0]: label} if (label and len(paths) == 1) else None
    return 0 if gate(paths, verb="no-keys", allow_dev_certs=allow,
                     labels=labels) else 1


if __name__ == "__main__":
    sys.exit(main())

"""Config-migration e2e — seed v1 → migrate → assert v2 → rollback, LIVE.

Proves the OTA config-migration chain against a REAL etcd-backed per (the
composer rig's cluster etcd, 127.0.0.1:2379): the exact path an SWP overlay runs
on-device via `theia-migrate forward` (Snapshot + PerManager.MigrateBulk over
TIPC, per dlopen's the plugin) and rolls back via `theia-migrate rollback`
(RestoreSnapshot). This is the P3 the ota-config-migration ticket wanted; it can
only run where per is etcd-backed (NOT the bare boxter — hence the composer rig).

The scenario asserts the config KEYSPACE, not a log: the stored value's shape
digest must flip v1 → v2 (the add-field migration rewrote the bytes), and a
rollback must restore the v1 digest.

Prereqs (a scenario sets these up, or a fixture): a live rig with per against
etcd; a v1 config seeded (tools/migrate/seed.py); a built migration plugin +
migration.json in a run dir; theia-migrate on PATH. Env:
  CM_MIGRATION_DIR   the dir with migration.json + the plugin .so
  CM_ETCD_CONTAINER  the etcd container name (default theia-etcd)
  CM_CONFIG_KEY      the etcd key to assert (default /theia/config/counter)
  CM_V1_DIGEST / CM_V2_DIGEST   the expected shape digests
  THEIA_MIGRATE      path to the theia-migrate binary
"""
from __future__ import annotations

import os
import subprocess

from robot.api import logger
from robot.api.deco import keyword, library


@library(scope="SUITE")
class ConfigMigrationLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self) -> None:
        self._dir = os.environ.get("CM_MIGRATION_DIR", "migration/run")
        self._etcd = os.environ.get("CM_ETCD_CONTAINER", "theia-etcd")
        self._key = os.environ.get("CM_CONFIG_KEY", "/theia/config/counter")
        self._v1 = os.environ.get("CM_V1_DIGEST", "")
        self._v2 = os.environ.get("CM_V2_DIGEST", "")
        self._migrate = os.environ.get("THEIA_MIGRATE", "theia-migrate")

    # ── drive the migration (the on-device overlay path) ────────────────────
    @keyword("Migrate Forward")
    def migrate_forward(self) -> str:
        """theia-migrate forward — Snapshot(pre-<artifact>) + MigrateBulk per
        step on the live per. Exactly what an SWP overlay install runs."""
        out = self._run([self._migrate, "forward", self._dir])
        logger.info(f"migrate forward:\n{out}")
        return out

    @keyword("Migrate Rollback")
    def migrate_rollback(self) -> str:
        """theia-migrate rollback — RestoreSnapshot(pre-<artifact>): the config
        rollback a PHM-unhealthy update triggers alongside the SW rollback."""
        out = self._run([self._migrate, "rollback", self._dir])
        logger.info(f"migrate rollback:\n{out}")
        return out

    # ── assert the etcd keyspace shape digest ───────────────────────────────
    @keyword("Config Digest Should Be V1")
    def digest_should_be_v1(self) -> None:
        self._assert_digest(self._v1, "v1")

    @keyword("Config Digest Should Be V2")
    def digest_should_be_v2(self) -> None:
        self._assert_digest(self._v2, "v2")

    @keyword("Config Should Contain")
    def config_should_contain(self, needle: str) -> None:
        """The stored config bytes must contain a substring (e.g. a preserved
        string field like the counter's label) — proves the migration reshaped
        WITHOUT dropping existing data."""
        raw = self._etcd_get_raw()
        if needle.encode() not in raw:
            raise AssertionError(
                f"{self._key} does not contain {needle!r} after migration; "
                f"bytes={raw[:64]!r}")

    # ── internals ───────────────────────────────────────────────────────────
    def _assert_digest(self, expected: str, label: str) -> None:
        if not expected:
            raise AssertionError(
                f"no {label} digest set (CM_{label.upper()}_DIGEST)")
        raw = self._etcd_get_raw()
        # The per value framing is: <digest-string>\0<proto-bytes>. The digest is
        # the leading printable cfg_… token.
        import re
        m = re.search(rb"cfg_[0-9a-f]+", raw)
        got = m.group(0).decode() if m else "<none>"
        if got != expected:
            raise AssertionError(
                f"{self._key} shape digest is {got!r}, expected the {label} "
                f"digest {expected!r}")
        logger.info(f"config digest is {label} ({got}) as expected")

    def _etcd_get_raw(self) -> bytes:
        p = subprocess.run(
            ["docker", "exec", self._etcd, "etcdctl", "get", self._key,
             "--print-value-only"],
            capture_output=True, timeout=15)
        if p.returncode != 0:
            raise AssertionError(
                f"etcdctl get {self._key} failed: {p.stderr.decode()}")
        return p.stdout

    def _run(self, argv: list) -> str:
        p = subprocess.run(argv, capture_output=True, text=True, timeout=45)
        out = (p.stdout + p.stderr).strip()
        if p.returncode != 0:
            raise AssertionError(
                f"{' '.join(argv)} FAILED (exit {p.returncode}):\n{out}")
        return out

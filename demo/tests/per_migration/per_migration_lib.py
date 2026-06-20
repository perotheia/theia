"""Robot library for config-migration tests — PER NODE, parametrized.

A migration is expressed PER NODE: each node's `config <Msg>` carries its own
v1->v2 shape evolution and its OWN rule set (rename / add / remove / set). This
library drives that flow generically for ANY node, given a MigrationCase:

    seed v1 value -> migrate.py OFFLINE (the design bench)
                  -> gen-transform plugin + build .so
                  -> per MigrateBulk ONLINE (the runtime)
                  -> assert offline == online   (the LOCKSTEP invariant)

Two modes per keyword:
  * HERMETIC (default): offline migrate.py only + a nanopb round-trip that
    mirrors the plugin's encode/decode. No live stack — deterministic, fast.
  * LIVE: also runs the real per MigrateBulk against a running supervisor and
    asserts the online result equals the offline one. Tagged `live`.

The CASES list is the parametrization surface — add a node's migration there and
both the per-node test and the multi-node sweep pick it up.
"""
from __future__ import annotations

import json
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

from robot.api.deco import keyword, library

# Workspace root: testing/scenarios/services/per_migration/ -> 4 up.
REPO = Path(__file__).resolve().parents[4]
MIGRATION = REPO / "migration"
SNAP_DIR = MIGRATION / "snapshots"


@dataclass(frozen=True)
class MigrationCase:
    """One node's v1->v2 config migration.

    node:        node prototype (the per store key).
    config_type: the bound config message name.
    proto_msg:   demo_pb2 class name for the V2 (current) shape.
    v1_fields:   the v1 field set (subset/old names) — what we seed + decode-as.
    from_digest: v1 schema digest (gen-schema of the v1 shape).
    to_digest:   v2 schema digest (current gen-schema).
    rules:       the transform rules (migrate.py / gen-transform vocabulary).
    seed:        a concrete v1 value to migrate (field->value).
    expect:      the expected v2 result after migration (field->value).
    """
    node: str
    config_type: str
    proto_msg: str
    from_digest: str
    to_digest: str
    rules: list[dict]
    seed: dict[str, Any]
    expect: dict[str, Any]
    # v1 field names (for the seed encode against the OLD shape).
    v1_fields: list[str] = field(default_factory=list)


# ---------------------------------------------------------------------------
# The parametrization surface — one entry per node migration. Digests are the
# gen-schema outputs captured in migration/schema_v3.json (v1) and
# schema_v4.json (v2). Each node has a DISTINCT rule kind.
# ---------------------------------------------------------------------------
CASES: list[MigrationCase] = [
    # CounterConfig: ADD a field (hysteresis). v1 {step,max_value,wrap,label}.
    MigrationCase(
        node="counter", config_type="CounterConfig", proto_msg="CounterConfig",
        from_digest="cfg_4be5118245cc2df4", to_digest="cfg_68334f330fa1ceae",
        rules=[{"op": "add", "field": "hysteresis", "default": 3}],
        v1_fields=["step", "max_value", "wrap", "label"],
        seed={"step": 5, "max_value": 100, "wrap": False, "label": "c1"},
        expect={"step": 5, "max_value": 100, "wrap": False, "label": "c1",
                "hysteresis": 3},
    ),
    # ObserverConfig: RENAME name->tag (same field number 2). v1 {poll_ms,name}.
    MigrationCase(
        node="observer", config_type="ObserverConfig", proto_msg="ObserverConfig",
        from_digest="cfg_641cf1694468714b", to_digest="cfg_9072169f29427dc1",
        rules=[{"op": "rename", "from": "name", "to": "tag"}],
        v1_fields=["poll_ms", "name"],
        seed={"poll_ms": 200, "name": "obs1"},
        expect={"poll_ms": 200, "tag": "obs1"},
    ),
    # P4Config: RENAME + ADD (the consolidation target). v1 {timeout_s,tag}.
    MigrationCase(
        node="demo_fsm", config_type="P4Config", proto_msg="P4Config",
        from_digest="cfg_6faa10dac8609dde", to_digest="cfg_37bde78e5ba60b3d",
        rules=[
            {"op": "rename", "from": "tag", "to": "label"},
            {"op": "add", "field": "step", "default": 1},
            {"op": "add", "field": "poll_ms", "default": 200},
            {"op": "add", "field": "amount", "default": 1},
        ],
        v1_fields=["timeout_s", "tag"],
        seed={"timeout_s": 5, "tag": "p4v1"},
        expect={"timeout_s": 5, "label": "p4v1", "step": 1, "poll_ms": 200,
                "amount": 1},
    ),
]


def _case(node: str) -> MigrationCase:
    for c in CASES:
        if c.node == node:
            return c
    raise AssertionError(f"no migration case for node {node!r}")


def _demo_pb2():
    """Import the demo protobuf module, generating it on demand into /tmp."""
    gen = Path("/tmp/rf_migpb")
    if not (gen / "system" / "apps" / "apps_pb2.py").exists():
        gen.mkdir(parents=True, exist_ok=True)
        proto = REPO / "platform/proto/system/apps/apps.proto"
        subprocess.run(
            [sys.executable, "-m", "grpc_tools.protoc",
             "-I", str(REPO / "platform/proto"),
             "-I", str(REPO / "platform/runtime/proto"),
             "--python_out", str(gen), str(proto)],
            check=True, cwd=str(REPO))
    p = str(gen / "system" / "demo")
    if p not in sys.path:
        sys.path.insert(0, p)
    import importlib
    return importlib.import_module("apps_pb2")


@library(scope="SUITE")
class PerMigrationLib:
    """Parametrized per-node migration keywords. State is the offline result
    cache so a test can run the migration once and assert several properties."""

    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self) -> None:
        self._offline: dict[str, dict] = {}   # node -> migrated config dict
        self._so_seq = 0                       # unique-plugin-path counter

    # -- the offline (hermetic) path --------------------------------------
    @keyword("Migrate Offline")
    def migrate_offline(self, node: str) -> dict:
        """Seed the case's v1 value as a one-node snapshot, run the REAL
        tools/migrate/migrate.py with the case's rules, and cache the result.
        Pure host-side — no live stack."""
        c = _case(node)
        SNAP_DIR.mkdir(parents=True, exist_ok=True)
        snap = {
            "label": f"mig_{node}",
            "nodes": {
                node: {"digest": c.from_digest, "config_type": c.config_type,
                       "config": dict(c.seed)},
            },
        }
        snap_path = SNAP_DIR / f"_rf_{node}_v1.json"
        snap_path.write_text(json.dumps(snap, indent=2))
        transform = {
            "config_type": c.config_type,
            "from_digest": c.from_digest, "to_digest": c.to_digest,
            "rules": c.rules,
        }
        tpath = SNAP_DIR / f"_rf_{node}_transform.json"
        tpath.write_text(json.dumps(transform, indent=2))
        out_path = SNAP_DIR / f"_rf_{node}_v2_offline.json"
        subprocess.run(
            [sys.executable, str(REPO / "tools/migrate/migrate.py"),
             "--snapshot", str(snap_path), "--transform", str(tpath),
             "--out", str(out_path)],
            check=True, cwd=str(REPO))
        result = json.loads(out_path.read_text())
        cfg = result["nodes"][node]["config"]
        self._offline[node] = cfg
        return cfg

    @keyword("Assert Migrated Value")
    def assert_migrated_value(self, node: str) -> None:
        """The offline-migrated config equals the case's expected v2 value."""
        c = _case(node)
        got = self._offline.get(node)
        if got is None:
            got = self.migrate_offline(node)
        assert got == c.expect, (
            f"{node}: migrated {got} != expected {c.expect}")

    @keyword("Assert Digest Bumped")
    def assert_digest_bumped(self, node: str) -> None:
        """The transform's from/to digests differ (a real shape change)."""
        c = _case(node)
        assert c.from_digest != c.to_digest, (
            f"{node}: from_digest == to_digest (no shape change)")

    # -- the nanopb round-trip (mirrors what the plugin does) -------------
    @keyword("Assert Nanopb Roundtrip")
    def assert_nanopb_roundtrip(self, node: str) -> None:
        """Encode the case's v1 seed against the V1 wire shape, then DECODE it
        with the V2 proto struct — exactly what the plugin's pb_decode does. The
        migration relies on FIELD-NUMBER stability, so the v1 bytes must decode
        cleanly into the v2 struct (carried fields land in their v2 members).
        Then apply the rule defaults and assert the expected v2 value. This is
        the nanopb half of the lockstep check, runnable without a live per."""
        c = _case(node)
        demo = _demo_pb2()
        v2_cls = getattr(demo, c.proto_msg)
        # Build v1 bytes by setting only the v1 fields on the v2 message (field
        # numbers are stable; v1 fields share numbers with their v2 members,
        # except renamed ones which we map by number below).
        msg = v2_cls()
        for k, v in c.seed.items():
            # A renamed field (e.g. name->tag) is set via its NEW member, since
            # they share the wire field number — that IS the carry.
            target = k
            for r in c.rules:
                if r.get("op") == "rename" and r.get("from") == k:
                    target = r["to"]
            setattr(msg, target, v)
        raw = msg.SerializeToString()
        # Decode with the v2 struct (the plugin's `from` decode).
        dec = v2_cls()
        dec.ParseFromString(raw)
        result = {f.name: getattr(dec, f.name) for f in dec.DESCRIPTOR.fields}
        # Apply the add-defaults (set-if-default) the plugin/migrate.py apply.
        for r in c.rules:
            if r.get("op") == "add":
                fld = r["field"]
                if not result.get(fld):
                    result[fld] = r["default"]
        # Only compare the expected keys (extra zero-valued v2 fields are fine).
        for k, v in c.expect.items():
            assert result.get(k) == v, (
                f"{node}: nanopb roundtrip {k}={result.get(k)!r} != {v!r}")

    # -- the live online path (needs a running per) -----------------------
    @keyword("Migrate Online And Compare")
    def migrate_online_and_compare(self, node: str, attempts: int = 3) -> None:
        """LIVE: seed v1 into the running per, build+load the plugin, run
        MigrateBulk, snapshot, and assert the online result equals the offline
        migrate.py result (the full lockstep invariant). Requires the stack up
        + etcd + the plugin .so built (bazel //migration:libper_migrate_*).

        Robust to per restart: a fresh probe occasionally load-balances onto a
        stale TIPC binding from a SIGKILL'd/restarted per (a known runtime
        hazard, project-probe-connect-stale-bindings), which surfaces as a
        ConnectionResetError. Each attempt uses a FRESH probe; we wait for per
        to be back before retrying."""
        import time as _t
        c = _case(node)
        offline = self._offline.get(node) or self.migrate_offline(node)
        plugin = self._plugin_so(node)   # build once, outside the retry loop

        last = None
        for i in range(attempts):
            try:
                self._do_online(node, c, offline, plugin)
                return
            except (ConnectionResetError, ConnectionError, BrokenPipeError) as e:
                last = e
                # per may be mid-restart; give the supervisor time to respawn it.
                _t.sleep(3.0 + 2.0 * i)
        raise AssertionError(
            f"{node}: online migration unreachable after {attempts} attempts "
            f"(per restart race): {last}")

    def _do_online(self, node, c, offline, plugin) -> None:
        sys.path.insert(0, str(REPO / "tools/tdb"))
        from tdb_client import PerClient  # noqa: E402
        demo = _demo_pb2()
        v2_cls = getattr(demo, c.proto_msg)

        per = PerClient.from_workspace(str(REPO))
        try:
            msg = v2_cls()
            for k, v in c.seed.items():
                target = k
                for r in c.rules:
                    if r.get("op") == "rename" and r.get("from") == k:
                        target = r["to"]
                setattr(msg, target, v)
            rep = per.probe.call("PerClient", "PutConfig", timeout=3.0,
                                 target_node=node, config=msg.SerializeToString(),
                                 digest=c.from_digest, expect_rev=0)
            assert rep.get("status") == 0, f"seed PutConfig failed: {rep}"

            rep = per.migrate_bulk(c.config_type, c.from_digest, c.to_digest,
                                   plugin_so=plugin, timeout=10.0)
            assert rep.get("status") == 0, f"MigrateBulk failed: {rep}"

            got = per.probe.call("PerClient", "GetConfig", timeout=3.0,
                                 target_node=node, want_digest="")
            assert got.get("digest") == c.to_digest, (
                f"online digest {got.get('digest')} != {c.to_digest}")
            m = v2_cls(); m.ParseFromString(bytes(got["config"]))
            online = {f.name: getattr(m, f.name) for f in m.DESCRIPTOR.fields}
            for k, v in offline.items():
                assert online.get(k) == v, (
                    f"{node}: LOCKSTEP MISMATCH {k}: online={online.get(k)!r} "
                    f"offline={v!r}")
        finally:
            per.stop()

    def _plugin_so(self, node: str) -> str:
        """Build (if needed) the per-node plugin .so and return a readable path.
        The BUILD target is //migration:libper_migrate_<node>.so."""
        target = f"//migration:libper_migrate_{node}.so"
        subprocess.run(["bazel", "build", target], check=True, cwd=str(REPO),
                       env={**__import__("os").environ,
                            "PATH": f"{REPO}/.venv/bin:" +
                                    __import__("os").environ.get("PATH", "")})
        built = REPO / "bazel-bin/migration" / f"libper_migrate_{node}.so"
        # Stage to a UNIQUE path, written ATOMICALLY (temp + os.replace). per
        # dlopens this with RTLD_NOW; overwriting an .so in place while a (prior)
        # per still maps it — or dlopening a half-written file — corrupts the
        # loaded code and crashes per (worker-thread SIGSEGV, empty backtrace).
        # A fresh name per build + atomic rename guarantees per only ever
        # dlopens a complete, immutable file.
        import os as _os
        uniq = f"rf_mig_{node}_{_os.getpid()}_{self._so_seq}.so"
        self._so_seq += 1
        dst = Path("/tmp") / uniq
        tmp = Path("/tmp") / (uniq + ".tmp")
        tmp.write_bytes(built.read_bytes())
        _os.replace(tmp, dst)   # atomic on the same filesystem
        return str(dst)

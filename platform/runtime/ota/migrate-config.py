#!/usr/bin/env python3
"""migrate-config.py — drive per's config migration during a Mender SWP update.

Shipped in the SWP artifact's migration/ part (staged by `theia release-swp
--migrate` as 00-migrate-config.py, so the theia-swp update module runs it
FIRST, before any hand-authored migration step). Reads migration.json sitting
NEXT TO this file:

    {"artifact": "<app>-<ver>-<abi>",
     "steps": [{"config_type": "...", "from_digest": "cfg_...",
                "to_digest": "cfg_...", "plugin": "libper_migrate_<node>.so",
                "transform": "<node>_v1_to_v2.json"}, ...]}

Forward (ArtifactInstall, still-old release running, non-zero ABORTS → Mender
rolls back):
  1. PerManager.Snapshot(label="pre-<artifact>")     — the rollback anchor;
  2. per step: PerManager.MigrateBulk(config_type, from, to, plugin_so=<abs
     .so path>) — per dlopen's the plugin, registers the edge, CAS-rewrites
     every stored value at `from` (incl. per-instance <component>/<N> clone
     keys). Idempotent: repeats/other machines find 0 at `from`;
  3. copy the .so(s) → $THEIA_ROOT/current/migrations/ so GetConfig's lazy
     migrate-on-read keeps the edge across per restarts until every straggler
     converges.

Rollback (ArtifactRollback → `--rollback`):
  PerManager.RestoreSnapshot("pre-<artifact>") — the ONLY safe reverse (there
  is no reverse edge). No snapshot / no per → non-zero, LOUD: rolled-back SW
  with migrated config is the one inconsistent state.

Speaks the gen_server wire via artheia.probe from $THEIA_ROOT/artheia — the
same env ucm-adopt.py uses; NEVER raw TIPC. A box without per in its tree
(probe connect fails after the retry budget) fails the install: a --migrate
SWP declares it NEEDS config migration, so "no per" is not a pass.

Usage:  migrate-config.py <THEIA_ROOT> [--rollback]
"""
from __future__ import annotations

import json
import os
import shutil
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
MANIFEST = HERE / "migration.json"

# How long to wait for per (it may be mid-restart while the update lands).
PER_RETRY_S = int(os.environ.get("THEIA_MIGRATE_PER_WAIT_S", "30"))


def _per_client(root: Path):
    """A PerManager probe via $THEIA_ROOT/artheia + tdb.art (the on-device
    probe env every provisioned rig carries). Returns (ctx, probe)."""
    sys.path.insert(0, str(root / "artheia"))
    from artheia.gen_server.probe import ArtheiaContext

    art = root / "system" / "tools" / "tdb" / "tdb.art"
    proto = root / "platform" / "proto"
    if not art.is_file():
        raise FileNotFoundError(f"no {art} — runtime manifest not provisioned")
    ctx = ArtheiaContext(str(art), str(proto))
    probe = ctx.probe("TdbPer", instance=(os.getpid() & 0x7FFFFFFF)).start()
    return ctx, probe


def _call_with_retry(probe, op: str, budget_s: int = PER_RETRY_S, **fields) -> dict:
    """Call PerManager.<op>, retrying connect-class failures for budget_s."""
    deadline = time.time() + budget_s
    last: Exception | None = None
    while time.time() < deadline:
        try:
            return probe.call("PerManager", op, timeout=5.0, **fields)
        except Exception as e:  # noqa: BLE001 — per may still be starting
            last = e
            time.sleep(2.0)
    raise RuntimeError(f"PerManager.{op} unreachable after {budget_s}s: {last}")


def forward(root: Path, doc: dict) -> int:
    label = f"pre-{doc['artifact']}"
    steps = doc.get("steps", [])
    if not steps:
        print("migrate-config: no steps — nothing to do")
        return 0
    _ctx, probe = _per_client(root)
    try:
        rep = _call_with_retry(probe, "Snapshot", label=label)
        if rep.get("status", 1) != 0:
            print(f"migrate-config: Snapshot({label}) REFUSED: "
                  f"{rep.get('message')}", file=sys.stderr)
            return 1
        print(f"migrate-config: snapshot '{label}' taken (rollback anchor)")

        for st in steps:
            so = HERE / st["plugin"]
            if not so.is_file():
                print(f"migrate-config: plugin {so} missing from payload",
                      file=sys.stderr)
                return 1
            rep = _call_with_retry(
                probe, "MigrateBulk",
                config_type=st["config_type"],
                from_digest=st["from_digest"], to_digest=st["to_digest"],
                plugin_so=str(so))
            if rep.get("status", 1) != 0:
                print(f"migrate-config: MigrateBulk {st['config_type']} "
                      f"{st['from_digest']}→{st['to_digest']} FAILED: "
                      f"{rep.get('message')}", file=sys.stderr)
                return 1
            print(f"migrate-config: {st['config_type']} "
                  f"{st['from_digest']}→{st['to_digest']}: {rep.get('message', 'ok')}")

        # Keep the edges alive across per restarts (lazy migrate-on-read for
        # stragglers): stash the plugins in the release tree. Best-effort — the
        # bulk pass above already converged everything reachable.
        dest = root / "current" / "migrations"
        try:
            dest.mkdir(parents=True, exist_ok=True)
            for st in steps:
                shutil.copy2(HERE / st["plugin"], dest / st["plugin"])
            print(f"migrate-config: plugins stashed in {dest}")
        except Exception as e:  # noqa: BLE001
            print(f"migrate-config: plugin stash failed ({e}) — non-fatal")
        return 0
    finally:
        probe.stop()


def rollback(root: Path, doc: dict) -> int:
    label = f"pre-{doc['artifact']}"
    _ctx, probe = _per_client(root)
    try:
        rep = _call_with_retry(probe, "RestoreSnapshot", label=label)
        if rep.get("status", 1) != 0:
            print(f"migrate-config: RestoreSnapshot({label}) FAILED: "
                  f"{rep.get('message')} — CONFIG MAY BE AHEAD OF THE "
                  f"ROLLED-BACK SW", file=sys.stderr)
            return 1
        print(f"migrate-config: config restored from snapshot '{label}'")
        # Drop the stashed plugins — the rolled-back release must not keep
        # lazily re-applying the abandoned edge.
        dest = root / "current" / "migrations"
        for st in doc.get("steps", []):
            try:
                (dest / st["plugin"]).unlink(missing_ok=True)
            except Exception:  # noqa: BLE001
                pass
        return 0
    finally:
        probe.stop()


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else
                os.environ.get("THEIA_ROOT", "/opt/theia"))
    do_rollback = "--rollback" in sys.argv[2:]
    if not MANIFEST.is_file():
        print(f"migrate-config: no {MANIFEST} — nothing to do")
        return 0
    doc = json.loads(MANIFEST.read_text())
    try:
        return rollback(root, doc) if do_rollback else forward(root, doc)
    except Exception as e:  # noqa: BLE001 — a migration must fail LOUDLY
        print(f"migrate-config: {'rollback' if do_rollback else 'forward'} "
              f"FAILED: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""seed.py — first-boot seed of DECLARED config defaults into per (etcd).

Framework tool (lives in tools/migrate/, invoked by `theia start` for any
workspace). The old demo/migration/seed.py carried this as its `defaults`
action next to demo-only seed/change/get drivers; the demo retirement kept
only this generic half.

    seed.py defaults --defaults <gen-config-defaults.json> --schema <gen-schema.json>

For every config-bearing node in the defaults file that has NO stored value
yet, PutConfig its declared defaults (idempotent — an existing value is left
untouched). Resolves each config's proto class DYNAMICALLY via the probe
codec from `art_package` + `proto_type` (gen-config-defaults emits both), so
it seeds EVERY FC / app config with no hardcoded class list.

Per-INSTANCE keys (`<component>/<instance>`, e.g. counter/1): the defaults
file is keyed by NODE — this seeds the BASE `<component>` key only. A clone
stores its own `<component>/<instance>` key on first write; until then it
reads/watches against a keyspace where only the base key exists, which is
exactly the declared-defaults baseline this seeds. (Snapshot/migration
tooling DOES understand instance-suffixed keys — see
tools/tdb/tdb_client.py decode_snapshot.)
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Framework checkout root = two dirs up (tools/migrate/seed.py). The probe
# client + codec live in tools/tdb/ and artheia/ of THIS checkout, regardless
# of which workspace the seed targets.
_FRAMEWORK = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(_FRAMEWORK / "tools" / "tdb"))

from tdb_client import PerClient  # noqa: E402


def seed_defaults(per, cfgdef: dict) -> int:
    """PutConfig declared defaults for every node with no stored value.

    Never lets one FC abort the sweep; returns the number seeded."""
    codec = per.ctx.codec
    seeded = 0
    for node, info in cfgdef.get("configs", {}).items():
        values = info.get("values") or {}
        # Seed EVERY config-bearing node, even one with no field defaults
        # (values == {}): the node still gets a /theia/config/<node> key — an
        # empty proto + its digest — so the key is visible in the Table Viewer
        # and the node has a stored baseline to edit from.
        cur = per.probe.call("PerClient", "GetConfig", timeout=3.0,
                             target_node=node, want_digest="")
        if cur.get("mod_rev", 0):
            print(f"  {node}: already has a stored value — not seeding")
            continue
        art_pkg = info.get("art_package")
        proto_type = info.get("proto_type")
        if not (art_pkg and proto_type):
            print(f"  {node}: defaults entry lacks art_package/proto_type "
                  f"(regen with current gen-config-defaults) — skip")
            continue
        try:
            raw = codec.encode(art_pkg, proto_type, **values)
        except Exception as ex:  # noqa: BLE001
            print(f"  {node}: encode {info.get('config_type')} failed ({ex}) — skip")
            continue
        rep = per.probe.call("PerClient", "PutConfig", timeout=3.0,
                             target_node=node, config=raw,
                             digest=info["digest"], expect_rev=0)
        print(f"  seeded {node} ({info.get('config_type')}@{info['digest']}) "
              f"{values} -> {rep.get('status')}")
        seeded += 1
    return seeded


def main() -> int:
    ap = argparse.ArgumentParser(
        description="seed declared config defaults into per (first boot)")
    ap.add_argument("action", choices=["defaults"])
    ap.add_argument("--defaults", type=Path, required=True,
                    help="gen-config-defaults output JSON")
    ap.add_argument("--schema", type=Path,
                    help="gen-schema output (accepted for symmetry; the "
                         "defaults file carries everything the seed needs)")
    ap.add_argument("--workspace", type=Path, default=Path.cwd(),
                    help="workspace root the probe resolves against "
                         "(default: cwd)")
    a = ap.parse_args()

    cfgdef = json.loads(a.defaults.read_text())
    per = PerClient.from_workspace(str(a.workspace))
    try:
        seed_defaults(per, cfgdef)
        return 0
    finally:
        per.stop()


if __name__ == "__main__":
    raise SystemExit(main())

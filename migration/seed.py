#!/usr/bin/env python3
"""Seed / change / read demo node configs via services/per (PutConfig/GetConfig).

A thin migration-experiment driver: encodes CounterConfig / ObserverConfig /
IncrementerConfig with protobuf and PutConfig's them into the live per (etcd
backend), keyed by the schema digest gen-schema emitted. Used by the migration-
verification flow to seed v1 values, change some, and read them back.

Usage:
    python migration/seed.py seed   --schema migration/schema_v1.json
    python migration/seed.py change --schema migration/schema_v1.json
    python migration/seed.py get    --schema migration/schema_v1.json
"""
import argparse
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools" / "tdb"))
sys.path.insert(0, "/tmp/migpb/system/demo")

import demo_pb2 as demo  # noqa: E402
from tdb_client import PerClient  # noqa: E402

# node prototype -> (proto message class, v1 field values)
SEED = {
    "counter":     (demo.CounterConfig,
                    dict(step=5, max_value=100, wrap=False, label="demo-counter")),
    "observer":    (demo.ObserverConfig,
                    dict(poll_ms=200, name="demo-observer")),
    "incrementer": (demo.IncrementerConfig,
                    dict(amount=2, period_ms=300, enabled=True)),
    "demo_fsm":    (demo.P4Config,
                    dict(timeout_s=5, tag="p4-v1")),
}

# values to CHANGE to (the "change some values" step) — distinct, observable.
CHANGE = {
    "counter":     dict(step=7, max_value=250, wrap=True, label="counter-edited"),
    "observer":    dict(poll_ms=500, name="observer-edited"),
    "incrementer": dict(amount=9, period_ms=150, enabled=False),
}


def _digest_for(schema, config_type):
    return schema["configs"][config_type]["digest"]


def _config_type_for(node, schema):
    for ct, info in schema["configs"].items():
        if node in info["nodes"]:
            return ct
    raise KeyError(f"no config bound to node {node}")


def put(per, node, msg_cls, values, schema):
    ct = _config_type_for(node, schema)
    digest = _digest_for(schema, ct)
    msg = msg_cls(**values)
    raw = msg.SerializeToString()
    rep = per.probe.call("PerClient", "PutConfig", timeout=3.0,
                         target_node=node, config=raw, digest=digest,
                         expect_rev=0)
    print(f"  PutConfig {node} ({ct}@{digest}) {values} -> {rep}",
          file=sys.stderr)


def get(per, node, schema):
    ct = _config_type_for(node, schema)
    msg_cls = SEED[node][0]
    rep = per.probe.call("PerClient", "GetConfig", timeout=3.0,
                         target_node=node, want_digest="")
    raw = rep.get("config", b"")
    msg = msg_cls()
    if raw:
        msg.ParseFromString(raw if isinstance(raw, bytes) else bytes(raw))
    fields = {f.name: getattr(msg, f.name) for f in msg.DESCRIPTOR.fields}
    print(f"  {node} ({ct}) digest={rep.get('digest')!r} {fields}")
    return fields


def seed_defaults(per, cfgdef, schema):
    """First-boot seed: PutConfig the DECLARED config defaults (gen-config-
    defaults output) for every node that has NO stored value yet. A node with an
    existing value is left untouched (idempotent — defaults seed once). Nodes
    whose config declares no defaults (empty `values`) are skipped."""
    name_to_cls = {c.DESCRIPTOR.name: c for c in
                   (demo.CounterConfig, demo.ObserverConfig,
                    demo.IncrementerConfig, demo.P4Config)}
    for node, info in cfgdef["configs"].items():
        values = info.get("values") or {}
        if not values:
            continue
        # Skip if already present (don't clobber a real stored value).
        cur = per.probe.call("PerClient", "GetConfig", timeout=3.0,
                             target_node=node, want_digest="")
        if cur.get("mod_rev", 0):
            print(f"  {node}: already has a stored value — not seeding defaults")
            continue
        cls = name_to_cls.get(info["config_type"])
        if cls is None:
            print(f"  {node}: no proto class for {info['config_type']} — skip")
            continue
        msg = cls(**values)
        rep = per.probe.call("PerClient", "PutConfig", timeout=3.0,
                             target_node=node, config=msg.SerializeToString(),
                             digest=info["digest"], expect_rev=0)
        print(f"  seeded {node} ({info['config_type']}@{info['digest']}) "
              f"{values} -> {rep.get('status')}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["seed", "change", "get", "defaults"])
    ap.add_argument("--schema", type=Path,
                    help="gen-schema output (for seed/change/get)")
    ap.add_argument("--defaults", type=Path,
                    help="gen-config-defaults output (for the `defaults` action)")
    a = ap.parse_args()

    per = PerClient.from_workspace(str(REPO))
    try:
        if a.action == "defaults":
            cfgdef = json.loads(a.defaults.read_text())
            schema = json.loads(a.schema.read_text()) if a.schema else None
            seed_defaults(per, cfgdef, schema)
            return 0
        schema = json.loads(a.schema.read_text())
        if a.action == "seed":
            for node, (cls, vals) in SEED.items():
                put(per, node, cls, vals, schema)
        elif a.action == "change":
            for node, vals in CHANGE.items():
                put(per, node, SEED[node][0], vals, schema)
        elif a.action == "get":
            for node in SEED:
                get(per, node, schema)
    finally:
        per.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

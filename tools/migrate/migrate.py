#!/usr/bin/env python3
"""migrate.py — apply a transform rule-set to a decoded config snapshot.

  migrate.py --snapshot snap_n.json --transform t.json --out snap_n1.json

Given a DECODED snapshot (the `tdb get-snapshot` JSON: {label, nodes:{node:
{digest, config_type, config:{...}}}}) and a transform.json rule-set, reshape
every config of the transform's `config_type` from `from_digest` to `to_digest`,
producing the n+1 snapshot. This is the reference applier — gen-transform emits
C that does the SAME field ops on the proto bytes, so the two must agree.

transform.json:
  {
    "config_type": "CounterConfig",
    "from_digest": "cfg_aaa", "to_digest": "cfg_bbb",
    "rules": [
      {"op": "rename", "from": "old", "to": "new"},
      {"op": "add",    "field": "f", "default": <json value>},
      {"op": "remove", "field": "f"},
      {"op": "set",    "field": "f", "value": <json value>},
      {"op": "copy",   "from": "a", "to": "b"}
    ]
  }

Rules apply in order on the decoded field dict. (The rule vocabulary is
deliberately small for now; extend `apply_rule` + gen-transform together.)
"""
from __future__ import annotations

import argparse
import copy
import json
import sys
from pathlib import Path


def apply_rule(cfg: dict, rule: dict) -> None:
    """Apply ONE rule to a decoded config dict (in place)."""
    op = rule.get("op")
    if op == "rename":
        src, dst = rule["from"], rule["to"]
        if src in cfg:
            cfg[dst] = cfg.pop(src)
    elif op == "add":
        # add only if absent (don't clobber an existing value)
        cfg.setdefault(rule["field"], rule.get("default"))
    elif op == "remove":
        cfg.pop(rule["field"], None)
    elif op == "set":
        cfg[rule["field"]] = rule["value"]
    elif op == "copy":
        src, dst = rule["from"], rule["to"]
        if src in cfg:
            cfg[dst] = copy.deepcopy(cfg[src])
    else:
        raise ValueError(f"unknown rule op: {op!r}")


def apply_transform(snapshot: dict, transform: dict) -> dict:
    """Return a NEW snapshot with the transform applied to matching configs."""
    ct = transform["config_type"]
    frm = transform.get("from_digest")
    to = transform.get("to_digest")
    rules = transform.get("rules", [])

    out = copy.deepcopy(snapshot)
    n = 0
    for node, entry in out.get("nodes", {}).items():
        if entry.get("config_type") != ct:
            continue
        # Migrate only values at from_digest (or any, if from_digest omitted).
        if frm is not None and entry.get("digest") != frm:
            continue
        cfg = entry.get("config")
        if not isinstance(cfg, dict):
            continue   # undecoded / raw — can't reshape
        for r in rules:
            apply_rule(cfg, r)
        if to is not None:
            entry["digest"] = to
        n += 1
    out.setdefault("_migrate", {})
    out["_migrate"] = {"config_type": ct, "from": frm, "to": to, "migrated": n}
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="apply a transform to a snapshot")
    ap.add_argument("--snapshot", required=True, type=Path)
    ap.add_argument("--transform", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    a = ap.parse_args(argv)

    snapshot = json.loads(a.snapshot.read_text())
    transform = json.loads(a.transform.read_text())
    result = apply_transform(snapshot, transform)
    a.out.write_text(json.dumps(result, indent=2) + "\n")
    m = result["_migrate"]
    print(f"migrated {m['migrated']} {m['config_type']} configs "
          f"{m['from']} -> {m['to']} -> {a.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

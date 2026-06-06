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


# ---- JSONPath (a small jq-consistent subset) -------------------------------
#
# Paths are dotted object access: "$.a.b.c" (or bare "a.b.c", or "a"). No
# wildcards/filters — config messages are nested objects, not collections, so
# the subset that matters is nested field traversal. This keeps migrate.py and
# the generated C plugin (gen-transform) able to agree without a JSONPath dep.
_SENTINEL = object()


def _path_parts(path: str) -> list[str]:
    p = path[2:] if path.startswith("$.") else (path[1:] if path == "$" else path)
    return [seg for seg in p.split(".") if seg]


def path_get(obj, path: str):
    """Value at `path`, or _SENTINEL if any segment is missing."""
    cur = obj
    for seg in _path_parts(path):
        if not isinstance(cur, dict) or seg not in cur:
            return _SENTINEL
        cur = cur[seg]
    return cur


def path_set(obj: dict, path: str, value) -> None:
    """Set `path` to `value`, creating intermediate objects as needed."""
    parts = _path_parts(path)
    cur = obj
    for seg in parts[:-1]:
        nxt = cur.get(seg)
        if not isinstance(nxt, dict):
            nxt = {}
            cur[seg] = nxt
        cur = nxt
    cur[parts[-1]] = value


def path_del(obj: dict, path: str) -> None:
    parts = _path_parts(path)
    cur = obj
    for seg in parts[:-1]:
        cur = cur.get(seg)
        if not isinstance(cur, dict):
            return
    cur.pop(parts[-1], None)


def apply_rule(cfg: dict, rule: dict) -> None:
    """Apply ONE rule to a decoded config dict (in place). All field references
    are JSONPath ($.a.b), so rules reach nested config fields."""
    op = rule.get("op")
    if op == "rename":
        src, dst = rule["from"], rule["to"]
        v = path_get(cfg, src)
        if v is not _SENTINEL:
            path_del(cfg, src)
            path_set(cfg, dst, v)
    elif op == "add":
        # add only if absent (don't clobber an existing value)
        field = rule.get("field") or rule.get("path")
        if path_get(cfg, field) is _SENTINEL:
            path_set(cfg, field, rule.get("default"))
    elif op == "remove":
        path_del(cfg, rule.get("field") or rule.get("path"))
    elif op == "set":
        path_set(cfg, rule.get("field") or rule.get("path"), rule["value"])
    elif op == "copy":
        src, dst = rule["from"], rule["to"]
        v = path_get(cfg, src)
        if v is not _SENTINEL:
            path_set(cfg, dst, copy.deepcopy(v))
    elif op == "transform":
        # Value/enum remap: replace the value at `path` via a mapping table.
        # Unmapped values pass through unchanged unless `default` is given.
        path = rule.get("path") or rule.get("field")
        mapping = rule.get("map") or rule.get("expression") or {}
        v = path_get(cfg, path)
        if v is not _SENTINEL:
            key = str(v)
            if key in mapping:
                path_set(cfg, path, mapping[key])
            elif "default" in rule:
                path_set(cfg, path, rule["default"])
    else:
        raise ValueError(f"unknown rule op: {op!r}")


def validate_cardinality(transform: dict) -> None:
    """per is a CONFIG gatekeeper: exactly ONE config per node (1 key = 1
    object). So config migration is strictly 1:1 — payload evolution within a
    node's single config. Storage-topology ops that change cardinality
    (moveKey / split / merge, fan-out) are OUT OF SCOPE here (they belong to a
    general document store, not per — see docs/artheia/transform.md). We REJECT
    any transform/op that declares non-1:1 cardinality or a maxFanout > 1, so an
    accidental fan-out can't silently explode the keyspace.
    """
    topo_ops = {"moveKey", "split", "merge", "fanout"}
    def bad_card(c):
        return c is not None and str(c) not in ("1:1", "1", "1to1")
    if bad_card(transform.get("cardinality")):
        raise ValueError(
            f"transform cardinality {transform['cardinality']!r} not allowed: "
            "per config migration is 1:1 (topology ops are out of scope)")
    mf = transform.get("maxFanout")
    if mf is not None and int(mf) > 1:
        raise ValueError(f"maxFanout {mf} > 1 not allowed: per is 1:1")
    for r in transform.get("rules", []):
        if r.get("op") in topo_ops:
            raise ValueError(
                f"op {r['op']!r} (storage topology) is out of scope for per: "
                "config is one key per node, no key moves / splits / merges")
        if bad_card(r.get("cardinality")):
            raise ValueError(
                f"rule cardinality {r['cardinality']!r} not allowed (1:1 only)")


def apply_transform(snapshot: dict, transform: dict) -> dict:
    """Return a NEW snapshot with the transform applied to matching configs."""
    validate_cardinality(transform)
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

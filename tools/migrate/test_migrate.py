"""Unit tests for migrate.py (the reference transform applier)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from migrate import apply_rule, apply_transform  # noqa: E402


def test_rules():
    cfg = {"a": 1, "label": "x"}
    apply_rule(cfg, {"op": "rename", "from": "label", "to": "tag"})
    assert cfg == {"a": 1, "tag": "x"}
    apply_rule(cfg, {"op": "add", "field": "n", "default": 7})
    assert cfg["n"] == 7
    apply_rule(cfg, {"op": "add", "field": "n", "default": 99})  # no clobber
    assert cfg["n"] == 7
    apply_rule(cfg, {"op": "set", "field": "a", "value": 5})
    assert cfg["a"] == 5
    apply_rule(cfg, {"op": "copy", "from": "a", "to": "a2"})
    assert cfg["a2"] == 5
    apply_rule(cfg, {"op": "remove", "field": "a2"})
    assert "a2" not in cfg


def test_apply_transform_digest_filter():
    snap = {"nodes": {
        "n1": {"digest": "old", "config_type": "C", "config": {"x": 1}},
        "n2": {"digest": "new", "config_type": "C", "config": {"x": 2}},  # skip (digest)
        "n3": {"digest": "old", "config_type": "Other", "config": {"x": 3}},  # skip (type)
    }}
    t = {"config_type": "C", "from_digest": "old", "to_digest": "new2",
         "rules": [{"op": "set", "field": "x", "value": 9}]}
    out = apply_transform(snap, t)
    assert out["nodes"]["n1"]["config"]["x"] == 9
    assert out["nodes"]["n1"]["digest"] == "new2"
    assert out["nodes"]["n2"]["config"]["x"] == 2   # untouched (digest filter)
    assert out["nodes"]["n3"]["config"]["x"] == 3   # untouched (type filter)
    assert out["_migrate"]["migrated"] == 1

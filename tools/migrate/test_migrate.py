"""Unit tests for migrate.py (the reference transform applier)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pytest  # noqa: E402

from migrate import (  # noqa: E402
    apply_rule, apply_transform, path_get, path_set, path_del,
    validate_cardinality, _SENTINEL,
)


def test_jsonpath_nested():
    cfg = {"a": {"b": {"c": 1}}}
    assert path_get(cfg, "$.a.b.c") == 1
    assert path_get(cfg, "$.a.x") is _SENTINEL
    path_set(cfg, "$.a.b.d", 2)
    assert cfg["a"]["b"]["d"] == 2
    path_set(cfg, "$.new.deep", 9)        # creates intermediates
    assert cfg["new"]["deep"] == 9
    path_del(cfg, "$.a.b.c")
    assert "c" not in cfg["a"]["b"]


def test_rename_and_transform_nested():
    cfg = {"address": {"city": "berlin"}, "status": "ACTIVE"}
    apply_rule(cfg, {"op": "rename", "from": "$.address.city",
                     "to": "$.location.city"})
    assert cfg["location"]["city"] == "berlin"
    apply_rule(cfg, {"op": "transform", "path": "$.status",
                     "map": {"ACTIVE": "enabled"}})
    assert cfg["status"] == "enabled"
    # unmapped value with a default
    apply_rule(cfg, {"op": "transform", "path": "$.status",
                     "map": {"NOPE": "x"}, "default": "fallback"})
    assert cfg["status"] == "fallback"


def test_cardinality_rejects_topology():
    with pytest.raises(ValueError):
        validate_cardinality({"rules": [{"op": "split"}]})
    with pytest.raises(ValueError):
        validate_cardinality({"cardinality": "1:N", "rules": []})
    with pytest.raises(ValueError):
        validate_cardinality({"maxFanout": 5, "rules": []})
    # 1:1 is fine
    validate_cardinality({"cardinality": "1:1", "rules": []})


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

"""Unit tests for the release-swp config-migration gate (OTA config migration).

The gate refuses a --migrate release unless every CHANGED config digest has a
reviewed, digest-matching transform in apps/<app>/migrations/."""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import theia  # noqa: E402


def _schema(digests: dict) -> dict:
    return {"configs": {ct: {"digest": d, "nodes": [ct.lower()]}
                        for ct, d in digests.items()}}


def _setup(tmp_path, monkeypatch, transforms: dict | None = None):
    monkeypatch.setattr(theia, "WORKSPACE", tmp_path)
    mig = tmp_path / "apps" / "demo" / "migrations"
    mig.mkdir(parents=True)
    for name, doc in (transforms or {}).items():
        (mig / name).write_text(json.dumps(doc))
    old = tmp_path / "old.json"
    new = tmp_path / "new.json"
    return old, new


def test_gate_no_changes(tmp_path, monkeypatch):
    old, new = _setup(tmp_path, monkeypatch)
    old.write_text(json.dumps(_schema({"C": "cfg_a"})))
    new.write_text(json.dumps(_schema({"C": "cfg_a"})))
    steps, errs = theia._config_migration_gate("demo", old, new)
    assert steps == [] and errs == []


def test_gate_missing_transform_refuses(tmp_path, monkeypatch):
    old, new = _setup(tmp_path, monkeypatch)
    old.write_text(json.dumps(_schema({"C": "cfg_a"})))
    new.write_text(json.dumps(_schema({"C": "cfg_b"})))
    steps, errs = theia._config_migration_gate("demo", old, new)
    assert not steps and len(errs) == 1 and "no transform" in errs[0]


def test_gate_stale_digests_refuse(tmp_path, monkeypatch):
    t = {"c_v1_to_v2.json": {"config_type": "C", "from_digest": "cfg_OLD",
                             "to_digest": "cfg_b", "rules": []}}
    old, new = _setup(tmp_path, monkeypatch, t)
    old.write_text(json.dumps(_schema({"C": "cfg_a"})))
    new.write_text(json.dumps(_schema({"C": "cfg_b"})))
    steps, errs = theia._config_migration_gate("demo", old, new)
    assert not steps and "STALE" in errs[0]


def test_gate_unresolved_review_refuses(tmp_path, monkeypatch):
    t = {"c_v1_to_v2.json": {"config_type": "C", "from_digest": "cfg_a",
                             "to_digest": "cfg_b",
                             "rules": [{"op": "rename", "from": "x", "to": "y",
                                        "_review": "same-tag rename guess"}]}}
    old, new = _setup(tmp_path, monkeypatch, t)
    old.write_text(json.dumps(_schema({"C": "cfg_a"})))
    new.write_text(json.dumps(_schema({"C": "cfg_b"})))
    steps, errs = theia._config_migration_gate("demo", old, new)
    assert not steps and "_review" in errs[0]


def test_gate_reviewed_transform_passes(tmp_path, monkeypatch):
    t = {"c_v1_to_v2.json": {"config_type": "C", "from_digest": "cfg_a",
                             "to_digest": "cfg_b",
                             "rules": [{"op": "rename", "from": "x", "to": "y"}]}}
    old, new = _setup(tmp_path, monkeypatch, t)
    old.write_text(json.dumps(_schema({"C": "cfg_a", "D": "cfg_same"})))
    new.write_text(json.dumps(_schema({"C": "cfg_b", "D": "cfg_same"})))
    steps, errs = theia._config_migration_gate("demo", old, new)
    assert errs == []
    assert len(steps) == 1
    st = steps[0]
    assert st["config_type"] == "C" and st["node"] == "c"
    assert st["plugin"] == "libper_migrate_c.so"
    assert (st["from_digest"], st["to_digest"]) == ("cfg_a", "cfg_b")


def test_gate_new_only_config_skipped(tmp_path, monkeypatch):
    old, new = _setup(tmp_path, monkeypatch)
    old.write_text(json.dumps(_schema({})))
    new.write_text(json.dumps(_schema({"FRESH": "cfg_x"})))
    steps, errs = theia._config_migration_gate("demo", old, new)
    assert steps == [] and errs == []

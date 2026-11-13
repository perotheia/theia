"""theia tap (tools/tap/tap.py): name resolution, the JSON <-> wire codec, info.

Hermetic: no TIPC, no RouDi. The live record/replay checks are in the step 7
report (docs/HOST-PASCAL.md, "The tap").
"""
import base64
import json
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent / "tap"))
import tap  # noqa: E402

THEIA = Path(__file__).resolve().parents[1]


def _pkg(root: Path, pkg: str, body: str) -> None:
    d = root / "system" / Path(*pkg.split(".")[1:])
    d.mkdir(parents=True, exist_ok=True)
    (d / "package.art").write_text(f"package {pkg}\n{body}\n")


@pytest.fixture
def ws(tmp_path):
    _pkg(tmp_path, "system.alpha", """
message TapPose { double x  double y }          // a comment: message TapFake {
message TapTwist { double v }
interface senderReceiver PoseFeed { data TapPose pose }
interface senderReceiver Motion { data TapPose pose  data TapTwist twist }
interface senderReceiver Shared { data TapPose pose }
""")
    _pkg(tmp_path, "system.beta", """
import system.alpha.*
message TapTwist { float v }
extern interface senderReceiver PoseFeed { data TapPose pose }
interface senderReceiver Shared { data TapTwist twist }
""")
    (tmp_path / "proto").mkdir()
    return tmp_path


def test_interface_resolves_to_its_group(ws):
    w, = tap.Index(ws).resolve("PoseFeed")      # beta's extern redeclaration is skipped
    assert (w.flat, w.art_package, w.interface) == ("system_alpha_TapPose", "system.alpha", "PoseFeed")
    assert w.proto_root == ws / "proto"


def test_interface_with_two_data_elements_is_two_groups(ws):
    assert [w.flat for w in tap.Index(ws).resolve("Motion")] == ["system_alpha_TapPose", "system_alpha_TapTwist"]


def test_same_named_interfaces_need_the_package(ws):
    ix = tap.Index(ws)
    with pytest.raises(SystemExit, match="system.alpha.Shared, system.beta.Shared"):
        ix.resolve("Shared")
    assert [w.flat for w in ix.resolve("system.beta.Shared")] == ["system_beta_TapTwist"]


def test_message_and_wire_type(ws):
    ix = tap.Index(ws)
    assert ix.resolve("TapPose")[0].flat == "system_alpha_TapPose"
    with pytest.raises(SystemExit, match="2 packages"):
        ix.resolve("TapTwist")                     # in alpha and beta
    assert ix.resolve("system_beta_TapTwist")[0].art_package == "system.beta"
    with pytest.raises(SystemExit):
        ix.resolve("NoSuch")


def test_framework_messages_resolve_with_the_framework_protos(tmp_path):
    w, = tap.Index(tmp_path).resolve("system_supervisor_HealthBeacon")
    assert w.proto_root == THEIA / "platform" / "proto"


def test_json_roundtrip_enum_bytes_uint64():
    w, = tap.Index(THEIA).resolve("system_services_log_TraceRecord")
    cls = tap._cls(w)
    m = cls(node_name="SmDaemon", ts_ns=(1 << 63) + 5, payload=b"\x00\xffwire", corr_id=7)
    m.kind = 1
    d = tap.to_json(m)
    assert d["ts_ns"] == (1 << 63) + 5 and isinstance(d["ts_ns"], int)
    assert base64.b64decode(d["payload"]) == b"\x00\xffwire"
    assert isinstance(d["kind"], str)            # enums by name
    json.dumps(d)                                # a jsonl line
    assert tap.from_json(w, d) == m.SerializeToString()


def test_info_counts_both_halves(tmp_path, capsys):
    (tmp_path / "system_x_M.jsonl").write_text(
        "".join(json.dumps({"rx_wall_ns": 1_000_000_000 * i, "msg": {}}) + "\n" for i in range(3)))
    (tmp_path / "streams.txt").write_text("W_s.bin /W/s\n")
    with open(tmp_path / "W_s.bin", "wb") as f:
        for i in range(4):
            f.write(struct.pack("<QII", 500_000_000 * i, 10, 0) + b"x" * 10)
    assert tap.main(["info", str(tmp_path)]) == 0
    out = json.loads(capsys.readouterr().out)
    assert out["tipc"]["system_x_M"] == {"messages": 3, "seconds": 2.0}
    assert out["rds"]["/W/s"] == {"chunks": 4, "bytes": 40, "seconds": 1.5}
    assert tap._rds_first_rx(tmp_path, []) == 0

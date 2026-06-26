#!/usr/bin/env python3
"""L4-B test helper — seed per-board PROVISIONAL activation markers via per.

Writes ucm_activation_<board> = UcmActivation{state=ACT_PROVISIONAL, ...} into the
SHARED etcd (through the central's per, the SOLE etcd client) for each board in
the roster — simulating each board's UCM reaching PROVISIONAL. V-UCM's
CMP_CONFIRMING barrier reads these markers; once ALL are PROVISIONAL it fans the
aggregate Confirm → VALIDATING → DONE.

Run ON the central (the probe reaches per at its local TIPC addr):
    python3 services/vucm/test/seed_markers.py <campaign_id> [board1 board2 ...]
Default boards: central compute
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "artheia"))

from artheia.gen_server.probe import ArtheiaContext  # noqa: E402

ART = REPO / "system/services/per/package.art"
PROTO = REPO / "platform/proto"

# UcmActivation field tags (system_services_ucm.UcmActivation):
#   state=1 (ActivationState: ACT_PROVISIONAL=1), version=2, campaign_id=3,
#   deadline_ns=4, scope=5. nanopb/proto3 varint+len-delim wire — hand-encode the
#   two fields V-UCM reads (state + campaign_id) so we don't need the gen proto here.
def _varint(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)

def encode_activation(state: int, campaign_id: str, version: str) -> bytes:
    b = bytearray()
    b += b"\x08" + _varint(state)                       # field 1 (state), varint
    v = version.encode()
    b += b"\x12" + _varint(len(v)) + v                  # field 2 (version), len-delim
    c = campaign_id.encode()
    b += b"\x1a" + _varint(len(c)) + c                  # field 3 (campaign_id), len-delim
    return bytes(b)

ACT_PROVISIONAL = 1

def main() -> int:
    campaign = sys.argv[1] if len(sys.argv) > 1 else "l4b-test-001"
    boards = sys.argv[2:] if len(sys.argv) > 2 else ["central", "compute"]
    ctx = ArtheiaContext(str(ART), proto_root=str(PROTO))
    probe = ctx.probe("PerManager").start()
    rc = 0
    try:
        payload = encode_activation(ACT_PROVISIONAL, campaign, "0.2.1")
        for board in boards:
            node = f"ucm_activation_{board}"
            r = probe.call("PerClient", "PutConfig", target_node=node,
                           config=payload, digest="prov", expect_rev=0)
            ok = r.get("status") == 0
            print(f"seed {node}: status={r.get('status')} rev={r.get('mod_rev')} {'OK' if ok else 'FAIL'}")
            if not ok:
                rc = 1
    finally:
        probe.stop()
    return rc

if __name__ == "__main__":
    raise SystemExit(main())

"""Robot-node selftest helpers (#387).

Builds FC signal payloads HOST-SIDE with the standard Python protobuf
lib (the libtrace_decoder FFI is no longer the encode path) and drives
the com robot node via the supdbg client. Wire format is identical to
the nanopb the sm daemon runs, so the bytes this builds are exactly what
sm's register_call decodes.
"""
from __future__ import annotations

import pathlib
import sys

from robot.api.deco import keyword, library

# supdbg + its generated stubs (bridge + FC _pb2) on sys.path.
_WS = pathlib.Path(__file__).resolve().parents[4]
_GEN = _WS / "tools" / "supdbg" / "_gen"
for p in (str(_WS / "tools"), str(_GEN), str(_GEN / "fc")):
    if p not in sys.path:
        sys.path.insert(0, p)


# sm's TIPC address — from services/system/sm/package.art
#   node atomic SmDaemon { tipc type=0x8001000D instance=0 }
SM_TIPC_TYPE = 0x8001000D
SM_TIPC_INSTANCE = 0

# nanopb C type name → djb2 service_id on the wire. Must match what
# sm's register_call<SmRequest, SmEmpty> computed.
SM_REQUEST_TYPE = "services_services_sm_SmRequest"


@library(scope="SUITE")
class RobotNodeLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    @keyword("Build Sm Request Payload")
    def build_sm_request_payload(self, target_state: str) -> bytes:
        """Encode an sm SmRequest{target=<state>} to proto-wire bytes
        with the standard python protobuf lib. `target_state` is an
        SmState enum name (OFF/STARTING/RUNNING/DEGRADED/UPDATE/SHUTDOWN).
        """
        from sm import sm_pb2  # _gen/fc/sm/sm_pb2.py
        state = getattr(sm_pb2, target_state.strip().upper())
        return sm_pb2.SmRequest(target=state).SerializeToString()

    @keyword("Sm Tipc Address")
    def sm_tipc_address(self) -> tuple[int, int]:
        return (SM_TIPC_TYPE, SM_TIPC_INSTANCE)

    @keyword("Sm Request Type Name")
    def sm_request_type_name(self) -> str:
        return SM_REQUEST_TYPE

    @keyword("Decode Sm Empty")
    def decode_sm_empty(self, payload: bytes) -> str:
        """Decode an SmEmpty reply (proves the reply round-trips)."""
        from sm import sm_pb2
        msg = sm_pb2.SmEmpty.FromString(bytes(payload))
        return str(msg)

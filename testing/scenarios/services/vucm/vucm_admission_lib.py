"""VucmGate admission-gate live probe (services/vucm).

Drives the AUTOSAR update-admission conjunction as a REAL COMPOSITION, over
TIPC, against a running VucmGate — the thing a unit test on admission_denied()
cannot reach: that the from_sm/from_nm/from_phm receivers are wired, that the
cast updates gate state, and that a RequestUpdate is then BLOCKED or ADMITTED
per the conjunction (SM parked ∧ NM link-ok ∧ PHM healthy ∧ garage window).

The probe casts the three foreign admission edges the way sm/nm/phm broadcast
them — SmStateMsg / NmStatusMsg / PhmHealthStatus — encoded host-side (pure
python protobuf, no FFI) and framed on the gen_server cast wire (GEN_CAST,
service_id = djb2_low16 of the nanopb C type name), then reads VucmGate's own
log to assert admit-vs-block. Mirrors the sm_central live-probe pattern.

Requires: a live supervisor with the services rig, VucmGate enforcing (config
enforce_sm/nm/phm=1 — a deploy/config/<machine>/vucm.json override), and
THEIA_TRACE / debug logging so the admission decision surfaces in the log.
Attach to the running rig; the probe never owns the supervisor.
"""
from __future__ import annotations

import os
import re
import socket
import struct
import time

from robot.api import logger
from robot.api.deco import keyword, library

# VucmGate — the admission gate (services/vucm/package.art).
VUCMGATE_TIPC_TYPE = 0x8001005E
VUCMGATE_TIPC_INSTANCE = 0

# The three admission-edge message types, by their nanopb C type name (the
# string djb2'd into the service_id — must match VucmGate's register_cast).
SM_STATE_TYPE = "system_services_sm_SmStateMsg"
NM_STATUS_TYPE = "system_services_nm_NmStatusMsg"
PHM_HEALTH_TYPE = "system_services_phm_PhmHealthStatus"

# Enum values (from the sm/nm/phm protos).
SM_RUNNING = 2          # SmState_RUNNING — the only admitting SM state
NET_OPERATIONAL = 6     # NetState_NETWORK_OPERATIONAL
NET_DEGRADED = 3        # NetState_DEGRADED (tunnel / bad link → block)
PHM_OK = 0              # HealthLevel OK
PHM_DEGRADED = 2        # HealthLevel_DEGRADED (unhealthy → block)

GEN_CAST = 0x20         # kMsgGenCast
GEN_CALL = 0x21         # kMsgGenCall
BUS_RPC = 2             # kBusTypeRpc

# VucmCtlIf.CheckForCampaign — the campaign entry (Mender → com → here). Server
# op: a CALL that drives EvDeployment → PLANNING → EvPlanned → AUTHORIZING, where
# admission_denied() runs. The request is CampaignRequest.
VUCM_CTL_IFACE = "system_services_vucm_CampaignRequest"
US_FULL = 0             # UpdateScope_US_FULL


def _djb2_low16(s: str) -> int:
    h = 5381
    for c in s.encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h & 0xFFFF


# ── pure-python protobuf encode (proto3, no libprotobuf / no FFI) ────────────
def _varint(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def _tag(field: int, wire: int) -> bytes:
    return _varint((field << 3) | wire)


def _enc_varint_field(field: int, value: int) -> bytes:
    if value == 0:
        return b""              # proto3 default — omitted
    return _tag(field, 0) + _varint(value)


def _sm_state_msg(state: int) -> bytes:
    # SmStateMsg { SmState state = 1; uint64 ts_ns = 2; }
    return _enc_varint_field(1, state)


def _nm_status_msg(state: int) -> bytes:
    # NmStatusMsg { NetState state = 1; string interface = 2; ... }
    return _enc_varint_field(1, state)


def _phm_health_status(level: int) -> bytes:
    # PhmHealthStatus { string entity = 1; HealthLevel level = 2; ... }
    return _enc_varint_field(2, level)


@library(scope="SUITE")
class VucmAdmissionLib:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self) -> None:
        # VucmGate logs to its own THEIA_LOGGER sink (file:/tmp/theia/vucm.log),
        # NOT the supervisor.log — the admission verdicts land there.
        self._log_path = os.environ.get("VUCM_LOG", "/tmp/theia/vucm.log")
        # Only lines AFTER this scenario started matter (the log persists across
        # runs); each block-assert scans from the campaign's own marker.
        self._since = 0

    # ── inject the three admission edges ────────────────────────────────────
    def _cast(self, type_name: str, payload: bytes) -> str:
        svc = _djb2_low16(type_name)
        # 24-byte TheiaMsgHeader: bus, msg, proto_len, ts(Q), then TheiaRpcMeta
        # (service_id H, method_id H, correlation_id I) + TipcMeta seq (H) + 2 pad.
        # Cast → correlation_id 0 (no reply).
        hdr = struct.pack("<BBHQHHIH2x", BUS_RPC, GEN_CAST, len(payload),
                          0, svc, 0, 0, 0)
        s = socket.socket(socket.AF_TIPC, socket.SOCK_SEQPACKET)
        s.connect((socket.TIPC_ADDR_NAME, VUCMGATE_TIPC_TYPE,
                   VUCMGATE_TIPC_INSTANCE, VUCMGATE_TIPC_INSTANCE,
                   socket.TIPC_NODE_SCOPE))
        s.sendall(hdr + payload)
        s.close()
        return payload.hex()

    @keyword("Start Campaign")
    def start_campaign(self, campaign_id: str = "rf-admission",
                       version: str = "1.0.0", timeout: float = 4.0) -> str:
        """Call VucmCtlIf.CheckForCampaign to kick a campaign — the entry that
        drives EvDeployment → PLANNING → EvPlanned → AUTHORIZING, where
        admission_denied() runs against the SM/NM/PHM state cast above. Returns
        the reply hex (CampaignReply{accepted, state}); the admission verdict is
        read from the log afterwards."""
        # CampaignRequest { string campaign_id=1; string version=2; UpdateScope scope=3 }
        payload = (self._str_field(1, campaign_id)
                   + self._str_field(2, version)
                   + _enc_varint_field(3, US_FULL))
        svc = _djb2_low16(VUCM_CTL_IFACE)
        corr = 0x5150         # any nonzero — the reply demux key
        # bus, msg, proto_len, ts(Q), svc(H), method(H), corr(I), seq(H) + 2 pad.
        hdr = struct.pack("<BBHQHHIH2x", BUS_RPC, GEN_CALL, len(payload),
                          0, svc, 0, corr, 0)
        s = socket.socket(socket.AF_TIPC, socket.SOCK_SEQPACKET)
        s.settimeout(timeout)
        s.connect((socket.TIPC_ADDR_NAME, VUCMGATE_TIPC_TYPE,
                   VUCMGATE_TIPC_INSTANCE, VUCMGATE_TIPC_INSTANCE,
                   socket.TIPC_NODE_SCOPE))
        s.sendall(hdr + payload)
        try:
            reply = s.recv(4096)
        except socket.timeout:
            reply = b""
        finally:
            s.close()
        logger.info(f"CheckForCampaign({campaign_id}) reply {len(reply)}B")
        return reply.hex()

    @staticmethod
    def _str_field(field: int, value: str) -> bytes:
        if not value:
            return b""
        b = value.encode()
        return _tag(field, 2) + _varint(len(b)) + b

    @keyword("Set Vehicle SM State")
    def set_sm_state(self, state: str = "RUNNING") -> str:
        """Cast SmStateMsg to VucmGate.from_sm. RUNNING admits; anything else
        (the vehicle moving / updating / shutting down) blocks."""
        val = SM_RUNNING if state.strip().upper() == "RUNNING" else 1
        return self._cast(SM_STATE_TYPE, _sm_state_msg(val))

    @keyword("Set Vehicle Network State")
    def set_net_state(self, state: str = "OPERATIONAL") -> str:
        """Cast NmStatusMsg to VucmGate.from_nm. OPERATIONAL admits; DEGRADED
        (a tunnel / bad link) blocks."""
        val = NET_DEGRADED if state.strip().upper() == "DEGRADED" \
            else NET_OPERATIONAL
        return self._cast(NM_STATUS_TYPE, _nm_status_msg(val))

    @keyword("Set Vehicle Health")
    def set_health(self, level: str = "OK") -> str:
        """Cast PhmHealthStatus to VucmGate.from_phm. OK admits; DEGRADED
        (unhealthy) blocks."""
        val = PHM_DEGRADED if level.strip().upper() == "DEGRADED" else PHM_OK
        return self._cast(PHM_HEALTH_TYPE, _phm_health_status(val))

    # ── read back the admission decision from VucmGate's log ────────────────
    @keyword("Admission Should Block")
    def admission_should_block(self, campaign_id: str, reason: str = "",
                               timeout: float = 8.0) -> str:
        """This campaign's AUTHORIZING leg must log 'admission BLOCKED' with the
        given reason substring. Scoped to the campaign_id so a re-checking
        neighbour can't satisfy it. Proves the conjunction denied THIS update."""
        pat = re.compile(r"campaign " + re.escape(campaign_id)
                         + r": admission BLOCKED — .*" + re.escape(reason))
        line = self._grep_log(pat, timeout)
        if line is None:
            raise AssertionError(
                f"VucmGate never logged an admission BLOCK for {campaign_id!r}"
                f"{f' matching {reason!r}' if reason else ''} within "
                f"{timeout}s; see {self._log_path}")
        logger.info(f"admission blocked as expected: {line.strip()}")
        return line

    @keyword("Admission Should Pass")
    def admission_should_pass(self, campaign_id: str,
                              timeout: float = 8.0) -> str:
        """This campaign must reach INSTALLING (the conjunction admitted, the gate
        fanned RequestUpdate) and must NOT have logged a BLOCK for its id."""
        blocked = re.compile(r"campaign " + re.escape(campaign_id)
                             + r": admission BLOCKED")
        b = self._grep_log(blocked, 1.5, expect_absent=True)
        if b is not None:
            raise AssertionError(
                f"expected {campaign_id!r} to PASS but VucmGate blocked it: "
                f"{b.strip()}")
        installing = re.compile(r"campaign " + re.escape(campaign_id)
                                + r": INSTALLING")
        line = self._grep_log(installing, timeout)
        if line is None:
            raise AssertionError(
                f"{campaign_id!r} admitted but never reached INSTALLING within "
                f"{timeout}s; see {self._log_path}")
        logger.info(f"admission passed → INSTALLING: {line.strip()}")
        return line

    @keyword("Reset Campaign")
    def reset_campaign(self, campaign_id: str) -> None:
        """RollbackCampaign to clear an in-flight/blocked campaign so the next
        test starts clean (a blocked campaign re-checks until cancelled)."""
        payload = self._str_field(1, campaign_id)   # RollbackRequest{campaign_id=1}
        svc = _djb2_low16("system_services_vucm_RollbackRequest")
        hdr = struct.pack("<BBHQHHIH2x", BUS_RPC, GEN_CALL, len(payload),
                          0, svc, 0, 0x5251, 0)
        try:
            s = socket.socket(socket.AF_TIPC, socket.SOCK_SEQPACKET)
            s.settimeout(3.0)
            s.connect((socket.TIPC_ADDR_NAME, VUCMGATE_TIPC_TYPE,
                       VUCMGATE_TIPC_INSTANCE, VUCMGATE_TIPC_INSTANCE,
                       socket.TIPC_NODE_SCOPE))
            s.sendall(hdr + payload)
            try:
                s.recv(4096)
            except socket.timeout:
                pass
            s.close()
        except OSError:
            pass
        time.sleep(0.5)

    # ── internals ───────────────────────────────────────────────────────────
    def _grep_log(self, pat: "re.Pattern", timeout: float,
                  expect_absent: bool = False):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            try:
                with open(self._log_path, "r", errors="replace") as f:
                    for line in f:
                        if pat.search(line):
                            if not expect_absent:
                                return line
                            last = line
            except FileNotFoundError:
                pass
            time.sleep(0.2)
        return last if expect_absent else None

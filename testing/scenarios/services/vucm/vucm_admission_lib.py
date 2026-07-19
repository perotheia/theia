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
        # NOT the supervisor.log — the admission verdicts land there. VUCM_LOG may
        # be a plain path OR "docker:<container>:<path>" to read the log live from
        # inside a container (the composer rig) via `docker exec cat` — no host
        # bind-mount / mirror needed, so no stale-history accumulation.
        self._log_path = os.environ.get("VUCM_LOG", "/tmp/theia/vucm.log")
        self._docker = None
        if self._log_path.startswith("docker:"):
            _, container, path = self._log_path.split(":", 2)
            self._docker = (container, path)
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

    # ── multi-board CMP_CONFIRMING barrier + garage auto-confirm ─────────────
    def _ucm_activation(self, state: int, campaign_id: str,
                        version: str) -> bytes:
        # UcmActivation { ActivationState state=1; string version=2;
        #                 string campaign_id=3; ... } — the two fields V-UCM's
        # barrier reads (state + campaign_id) plus version. state 1 = PROVISIONAL.
        return (_enc_varint_field(1, state)
                + self._str_field(2, version)
                + self._str_field(3, campaign_id))

    @keyword("Seed Board Provisional")
    def seed_board_provisional(self, board: str, campaign_id: str,
                               version: str = "1.0.0") -> int:
        """Write ucm_activation_<board> = UcmActivation{state=PROVISIONAL} into the
        SHARED etcd via per (PutConfig, target_node = the key), simulating that
        board's UcmDaemon reaching PROVISIONAL. V-UCM's CMP_CONFIRMING poll reads
        these; once EVERY roster board is PROVISIONAL the barrier completes."""
        payload = self._ucm_activation(1, campaign_id, version)  # 1 = ACT_PROVISIONAL
        node = f"ucm_activation_{board}"
        reply = self._per_put(node, payload, digest="prov")
        logger.info(f"seeded {node} PROVISIONAL for {campaign_id} → {reply}B reply")
        return reply

    @keyword("Clear Board Marker")
    def clear_board_marker(self, board: str) -> None:
        """Best-effort clear of ucm_activation_<board> (write NONE state=0) so a
        later campaign's barrier starts from a clean per keyspace."""
        try:
            self._per_put(f"ucm_activation_{board}", b"", digest="clear")
        except OSError:
            pass

    def _per_put(self, node: str, payload: bytes, digest: str) -> int:
        # PerClient.PutConfig { target_node=1; bytes config=2; string digest=3;
        #   uint32 expect_rev=4 } — the per node at TIPC 0x80010007 inst 0.
        PER_TIPC_TYPE = 0x80010007
        req = (self._str_field(1, node)
               + _tag(2, 2) + _varint(len(payload)) + payload
               + self._str_field(3, digest)
               + _enc_varint_field(4, 0))
        svc = _djb2_low16("system_services_per_PutConfigReq")
        hdr = struct.pack("<BBHQHHIH2x", BUS_RPC, GEN_CALL, len(req),
                          0, svc, 0, 0x7075, 0)
        s = socket.socket(socket.AF_TIPC, socket.SOCK_SEQPACKET)
        s.settimeout(4.0)
        s.connect((socket.TIPC_ADDR_NAME, PER_TIPC_TYPE, 0, 0,
                   socket.TIPC_NODE_SCOPE))
        s.sendall(hdr + req)
        try:
            reply = s.recv(4096)
        except socket.timeout:
            reply = b""
        finally:
            s.close()
        return len(reply)

    @keyword("Reset Vucm Log")
    def reset_vucm_log(self) -> None:
        """Truncate VucmGate's log so a suite's assertions can't match a stale
        line from a previous run (the log persists across runs). Works for the
        docker: scheme (truncate inside the container) and a plain path."""
        if self._docker is not None:
            import subprocess
            container, path = self._docker
            try:
                subprocess.run(["docker", "exec", container, "sh", "-c",
                                f": > {path}"], timeout=5)
            except (subprocess.SubprocessError, OSError):
                pass
            return
        try:
            open(self._log_path, "w").close()
        except OSError:
            pass

    @keyword("Commit Campaign")
    def commit_campaign(self, campaign_id: str, timeout: float = 4.0) -> str:
        """Call VucmCtlIf.CommitCampaign — the operator go that fans the aggregate
        Confirm to every board, driving CMP_AWAITING_COMMIT → VALIDATING → DONE.
        Valid only while the campaign holds at AWAITING_COMMIT (barrier complete)."""
        payload = self._str_field(1, campaign_id)   # CommitRequest{campaign_id=1}
        svc = _djb2_low16("system_services_vucm_CommitRequest")
        hdr = struct.pack("<BBHQHHIH2x", BUS_RPC, GEN_CALL, len(payload),
                          0, svc, 0, 0x434D, 0)
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
        logger.info(f"CommitCampaign({campaign_id}) reply {len(reply)}B")
        return reply.hex()

    @keyword("Barrier Should Await Commit")
    def barrier_should_await_commit(self, campaign_id: str, boards: int,
                                    timeout: float = 20.0) -> str:
        """The CMP_CONFIRMING barrier must observe ALL <boards> boards PROVISIONAL
        and then HOLD at AWAITING OPERATOR COMMIT (require_user_confirm path)."""
        pat = re.compile(r"campaign " + re.escape(campaign_id)
                         + r": ALL " + str(boards)
                         + r" boards PROVISIONAL — AWAITING OPERATOR COMMIT")
        line = self._grep_log(pat, timeout)
        if line is None:
            raise AssertionError(
                f"barrier for {campaign_id!r} never reached AWAITING COMMIT with "
                f"all {boards} boards PROVISIONAL within {timeout}s; "
                f"see {self._log_path}")
        logger.info(f"barrier complete → awaiting commit: {line.strip()}")
        return line

    @keyword("Barrier Must Not Complete")
    def barrier_must_not_complete(self, campaign_id: str, boards: int,
                                  timeout: float = 6.0) -> None:
        """The CMP_CONFIRMING barrier must NOT complete — with fewer than <boards>
        boards PROVISIONAL, no AWAITING COMMIT / garage line may appear within the
        window (proves the barrier really waits for EVERY board)."""
        pat = re.compile(r"campaign " + re.escape(campaign_id)
                         + r": ALL " + str(boards) + r" boards PROVISIONAL")
        line = self._grep_log(pat, timeout, expect_absent=True)
        if line is not None:
            raise AssertionError(
                f"barrier for {campaign_id!r} completed with < {boards} boards "
                f"PROVISIONAL — it did not wait for every board: {line.strip()}")
        logger.info(f"barrier correctly held (partial roster) for {campaign_id}")

    @keyword("Campaign Should Reach Done")
    def campaign_should_reach_done(self, campaign_id: str,
                                   timeout: float = 15.0) -> str:
        """After commit (operator or garage), the campaign must log DONE — all
        boards confirmed ACTIVE (VALIDATING → DONE)."""
        pat = re.compile(r"campaign " + re.escape(campaign_id)
                         + r": DONE — all boards confirmed ACTIVE")
        line = self._grep_log(pat, timeout)
        if line is None:
            raise AssertionError(
                f"campaign {campaign_id!r} never reached DONE within {timeout}s; "
                f"see {self._log_path}")
        logger.info(f"campaign DONE: {line.strip()}")
        return line

    @keyword("Garage Should Auto Confirm")
    def garage_should_auto_confirm(self, campaign_id: str, boards: int,
                                   timeout: float = 20.0) -> str:
        """In garage mode (auto_confirm_in_window + in-window), the barrier must
        auto-confirm — log 'ALL <boards> boards PROVISIONAL, in-window —
        AUTO-CONFIRM (garage)' WITHOUT holding for an operator commit."""
        pat = re.compile(r"campaign " + re.escape(campaign_id)
                         + r": ALL " + str(boards)
                         + r" boards PROVISIONAL, in-window — AUTO-CONFIRM \(garage\)")
        line = self._grep_log(pat, timeout)
        if line is None:
            raise AssertionError(
                f"garage auto-confirm never fired for {campaign_id!r} within "
                f"{timeout}s; see {self._log_path}")
        logger.info(f"garage auto-confirm: {line.strip()}")
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
    def _read_log_lines(self):
        if self._docker is not None:
            import subprocess
            container, path = self._docker
            try:
                out = subprocess.run(
                    ["docker", "exec", container, "cat", path],
                    capture_output=True, text=True, timeout=5)
                return out.stdout.splitlines()
            except (subprocess.SubprocessError, OSError):
                return []
        try:
            with open(self._log_path, "r", errors="replace") as f:
                return f.read().splitlines()
        except FileNotFoundError:
            return []

    def _grep_log(self, pat: "re.Pattern", timeout: float,
                  expect_absent: bool = False):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            for line in self._read_log_lines():
                if pat.search(line):
                    if not expect_absent:
                        return line
                    last = line
            time.sleep(0.2)
        return last if expect_absent else None

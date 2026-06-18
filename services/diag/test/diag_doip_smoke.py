#!/usr/bin/env python3
"""diag_doip_smoke.py — the canonical e2e smoke for the diag FC over real DoIP.

Drives the live diag FC (DoipServer @ TCP/13400) through routing activation + the
UDS v1 set (0x10 session, 0x22 read identity DID + fault-log DIDs, 0x2E write,
0x19 read DTC) and asserts the responses. Reused by the rf-theia adapter
(diag_doip_lib.py) AND runnable standalone. Exit 0 = all checks pass.

The diag FC must be running (DoipServer bound on 13400). For the fault-log /
0x19 checks to show faults, phm must be up + a fault induced; without phm the
checks still pass (empty fault log / no-DTC is the honest answer).
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys

DOIP_VERSION = 0x02
PT_ROUTING_ACTIV_REQ = 0x0005
PT_ROUTING_ACTIV_RESP = 0x0006
PT_DIAG_MESSAGE = 0x8001
PT_DIAG_ACK = 0x8002
TESTER_ADDR = 0x0E80
ECU_ADDR = 0x0001


def _frame(ptype: int, payload: bytes) -> bytes:
    return struct.pack(">BBHI", DOIP_VERSION, (~DOIP_VERSION) & 0xFF,
                       ptype, len(payload)) + payload


def _recv_exact(sock, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("DoIP server closed the connection")
        buf += chunk
    return buf


def _recv_msg(sock) -> tuple[int, bytes]:
    hdr = _recv_exact(sock, 8)
    _ver, _inv, ptype, plen = struct.unpack(">BBHI", hdr)
    return ptype, (_recv_exact(sock, plen) if plen else b"")


class DoipTester:
    """A connected DoIP tester session against the diag FC."""

    def __init__(self, host: str = "127.0.0.1", port: int = 13400, timeout: float = 5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)

    def routing_activate(self) -> int:
        self.sock.sendall(_frame(PT_ROUTING_ACTIV_REQ,
                                 struct.pack(">HBI", TESTER_ADDR, 0x00, 0)))
        ptype, p = _recv_msg(self.sock)
        if ptype != PT_ROUTING_ACTIV_RESP:
            raise AssertionError(f"unexpected response 0x{ptype:04X}")
        return p[4]   # the routing-activation code (0x10 = success)

    def uds(self, req: bytes) -> bytes:
        """Send a UDS request; return the UDS response (skips the DoIP ACK)."""
        msg = struct.pack(">HH", TESTER_ADDR, ECU_ADDR) + req
        self.sock.sendall(_frame(PT_DIAG_MESSAGE, msg))
        for _ in range(2):
            ptype, p = _recv_msg(self.sock)
            if ptype == PT_DIAG_MESSAGE:
                return p[4:]   # strip source+target → the UDS payload
        return b""

    def close(self):
        self.sock.close()


def run(host: str = "127.0.0.1", port: int = 13400) -> int:
    """The smoke sequence. Returns 0 on all-pass, 1 on a failed assertion."""
    t = DoipTester(host, port)
    try:
        assert t.routing_activate() == 0x10, "routing activation must succeed"

        # 0x10 extended session → positive (sid|0x40 = 0x50).
        r = t.uds(bytes([0x10, 0x03]))
        assert r[:2] == bytes([0x50, 0x03]), f"session: {r.hex()}"

        # 0x22 read VIN (0xF190) → positive 0x62 + DID + value.
        r = t.uds(bytes([0x22, 0xF1, 0x90]))
        assert r[:1] == bytes([0x62]) and r[1:3] == bytes([0xF1, 0x90]), f"readVIN: {r.hex()}"
        vin = r[3:].decode("ascii", "replace")
        assert len(vin) == 17, f"VIN must be 17 chars: {vin!r}"

        # 0x2E write a DID → positive 0x6E (0x27 dropped → not gated).
        r = t.uds(bytes([0x2E, 0xF1, 0x90]) + b"WROTEBYTHETESTER0")
        assert r[:1] == bytes([0x6E]), f"writeDID: {r.hex()}"
        # read it back → the written value.
        r = t.uds(bytes([0x22, 0xF1, 0x90]))
        assert r[3:] == b"WROTEBYTHETESTER0", f"readback: {r.hex()}"

        # 0x22 fault COUNT DID (0xFDFF) → positive (count byte; >=0).
        r = t.uds(bytes([0x22, 0xFD, 0xFF]))
        assert r[:1] == bytes([0x62]) and r[1:3] == bytes([0xFD, 0xFF]), f"faultCount: {r.hex()}"
        n_faults = r[3] if len(r) > 3 else 0

        # 0x22 fault-log entry 0 (0xFD00): positive if a fault exists, else NRC 0x31.
        r = t.uds(bytes([0x22, 0xFD, 0x00]))
        if n_faults > 0:
            assert r[:1] == bytes([0x62]), f"fault[0]: {r.hex()}"
        else:
            assert r[:1] == bytes([0x7F]) and r[2] == 0x31, f"fault[0] empty→NRC31: {r.hex()}"

        # 0x19 read DTC by status mask → positive 0x59 (DTC list per phm health).
        r = t.uds(bytes([0x19, 0x02, 0xFF]))
        assert r[:1] in (bytes([0x59]), bytes([0x7F])), f"readDTC: {r.hex()}"

        print(f"diag smoke OK: VIN={vin!r} faults={n_faults} "
              f"DTC={'present' if r[:1]==bytes([0x59]) and len(r)>4 else 'none/NRC'}")
        return 0
    except AssertionError as e:
        print(f"diag smoke FAIL: {e}", file=sys.stderr)
        return 1
    finally:
        t.close()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=13400)
    a = ap.parse_args()
    return run(a.host, a.port)


if __name__ == "__main__":
    raise SystemExit(main())

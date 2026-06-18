#!/usr/bin/env python3
"""doip_client.py — a minimal DoIP (ISO 13400) tester that drives the diag FC's
DoipServer over real TCP/13400 end-to-end (the full transport path, vs the probe
tester which hits UdsRouter directly over TIPC).

Implements just enough DoIP to: routing-activate, then send UDS diagnostic
messages + read the responses. Mirrors services/diag/impl/doip.hpp's framing.

Usage:
    doip_client.py [--host 127.0.0.1] [--port 13400]
Runs a session → read VIN → write (denied) → security → write (ok) → read DTC
sequence and prints each UDS response.
"""
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


def frame(ptype: int, payload: bytes) -> bytes:
    return struct.pack(">BBHI", DOIP_VERSION, (~DOIP_VERSION) & 0xFF,
                       ptype, len(payload)) + payload


def recv_msg(sock) -> tuple[int, bytes]:
    """Read one DoIP message; return (payload_type, payload)."""
    hdr = _recv_exact(sock, 8)
    ver, inv, ptype, plen = struct.unpack(">BBHI", hdr)
    payload = _recv_exact(sock, plen) if plen else b""
    return ptype, payload


def _recv_exact(sock, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("DoIP server closed the connection")
        buf += chunk
    return buf


def routing_activation(sock) -> bool:
    # source(2) + activation type(1) + reserved(4)
    payload = struct.pack(">HBI", TESTER_ADDR, 0x00, 0)
    sock.sendall(frame(PT_ROUTING_ACTIV_REQ, payload))
    ptype, p = recv_msg(sock)
    if ptype != PT_ROUTING_ACTIV_RESP:
        print(f"  unexpected response 0x{ptype:04X}", file=sys.stderr)
        return False
    code = p[4]
    print(f"  routing activation → code 0x{code:02X} "
          f"({'success' if code == 0x10 else 'denied'})")
    return code == 0x10


def uds(sock, req: bytes, label: str) -> bytes:
    """Send a UDS request in a DiagnosticMessage; return the UDS response."""
    msg = struct.pack(">HH", TESTER_ADDR, ECU_ADDR) + req
    sock.sendall(frame(PT_DIAG_MESSAGE, msg))
    # The server ACKs (0x8002) then sends the response diag-message.
    for _ in range(2):
        ptype, p = recv_msg(sock)
        if ptype == PT_DIAG_MESSAGE:
            resp = p[4:]   # strip source+target
            kind = "NRC" if resp[:1] == b"\x7f" else "positive"
            print(f"  {label}: req={req.hex()} → resp={resp.hex()} ({kind})")
            return resp
        # else it was the ACK — read the next frame.
    return b""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=13400)
    a = ap.parse_args()

    with socket.create_connection((a.host, a.port), timeout=5) as sock:
        print(f"connected to DoIP {a.host}:{a.port}")
        if not routing_activation(sock):
            return 1
        # 0x10 extended session.
        uds(sock, bytes([0x10, 0x03]), "session(extended)")
        # 0x22 read VIN (0xF190).
        uds(sock, bytes([0x22, 0xF1, 0x90]), "readDID(VIN)")
        # 0x2E write VIN — DENIED (no security yet, expect NRC 0x33).
        uds(sock, bytes([0x2E, 0xF1, 0x90]) + b"NEWVIN", "writeDID(no-sec)")
        # 0x27 request seed.
        seed_resp = uds(sock, bytes([0x27, 0x01]), "securityAccess(seed)")
        # 0x27 send key (a dummy key — crypto Verify will reject → NRC 0x35,
        # unless a real signed key is provided; this proves the seed/key path).
        uds(sock, bytes([0x27, 0x02]) + b"\x00\x00\x00\x00", "securityAccess(key)")
        # 0x19 read DTC by status mask (0x02) — depends on phm health.
        uds(sock, bytes([0x19, 0x02, 0xFF]), "readDTC(byMask)")
        print("DoIP sequence complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

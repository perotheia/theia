#!/usr/bin/env python3
"""S3_server timer smoke (ISO 14229) — live, over real DoIP/13400.

Proves the two halves of the S3 session keep-alive against the running
diag FC (sibling of diag_doip_smoke.py; same client, same transport):

  HOLD:   enter ExtendedDiagnosticSession (0x10 0x03), then beat
          TesterPresent (0x3E 0x00) every ~S3/2 for longer than S3 —
          the session must NOT revert (no S3 log line in the window).
  REVERT: go silent for > S3 — UdsRouter must revert to DefaultSession,
          observable as the "S3_server timeout" line in the node log
          (v1 has no session-gated UDS service to probe the revert on
          the wire; the log is the authoritative edge).

The S3 default is 5000ms (DiagConfig.session_timeout_ms); this smoke
assumes the default — a rig with a custom config needs S3_MS adjusted.

Run with the stack up (`theia start`). Exit 0 = both halves proven.
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from doip_client import routing_activation, uds  # noqa: E402

import socket  # noqa: E402

S3_MS = 5000
DIAG_LOG = Path("/tmp/theia/diag.log")
TIMEOUT_NEEDLE = "S3_server timeout"


def log_hits_since(mark: int) -> int:
    """Count S3-timeout log lines appended after byte offset `mark`."""
    if not DIAG_LOG.is_file():
        return 0
    data = DIAG_LOG.read_bytes()[mark:]
    return data.decode(errors="replace").count(TIMEOUT_NEEDLE)


def main() -> int:
    mark = DIAG_LOG.stat().st_size if DIAG_LOG.is_file() else 0

    s = socket.create_connection(("127.0.0.1", 13400), timeout=5)
    if not routing_activation(s):
        print("FAIL: routing activation")
        return 1

    r = uds(s, bytes([0x10, 0x03]), "0x10 ExtendedDiagnosticSession")
    if not r or r[0] != 0x50:
        print(f"FAIL: could not enter extended session: {r.hex()}")
        return 1

    # HOLD: 3 TesterPresent beats at ~S3/2 — spans > S3 total.
    beat_s = (S3_MS / 1000.0) / 2.5
    for _ in range(3):
        time.sleep(beat_s)
        r = uds(s, bytes([0x3E, 0x00]), "0x3E TesterPresent")
        if not r or r[0] != 0x7E:
            print(f"FAIL: TesterPresent rejected: {r.hex()}")
            return 1
    if log_hits_since(mark):
        print("FAIL: session reverted DURING the keep-alive window "
              "(S3 timer fired despite TesterPresent)")
        return 1
    print(f"HOLD ok — {3 * beat_s:.1f}s of beats > S3={S3_MS}ms, no revert")

    # REVERT: silence for S3 + margin; expect exactly the timeout edge.
    time.sleep(S3_MS / 1000.0 + 2.0)
    hits = log_hits_since(mark)
    if hits < 1:
        print("FAIL: no S3_server timeout after "
              f"{S3_MS + 2000}ms of tester silence — timer not armed?")
        return 1
    print(f"REVERT ok — S3_server timeout logged after silence (hits={hits})")

    s.close()
    print("S3 smoke PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())

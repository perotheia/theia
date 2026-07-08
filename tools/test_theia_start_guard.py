"""Unit tests for `theia start`'s cross-workspace supervisor collision guard.

Regression coverage for the failure where two supervisors from different dev
workspaces co-bound the SAME fixed TIPC address (0x80020001:<machine_instance>)
on one host. Because TIPC name binds are SEQPACKET-anycast, a probe (tdb ps /
GetTree) then landed nondeterministically on either supervisor, or timed out if
one was crash-looping — which read as a "tdb ps regression" but was actually an
address collision the per-workspace pidfile guard couldn't see.

`theia start` now fails fast: it reads the TIPC nametable and refuses to start if
the supervisor address is already published on the host. These tests exercise the
nametable parser (`_tipc_addr_bound`) against representative `tipc nametable show`
output, including the range (Lower..Upper) semantics and malformed rows.
"""
import sys
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pytest  # noqa: E402

import theia  # noqa: E402  (tools/theia.py)


# 0x80020001 = 2147614721 — the supervisor ctl TIPC type.
SUP_TYPE = 0x80020001

# A representative `tipc nametable show` with a supervisor bound at instance 0
# (Lower==Upper==0) plus an unrelated ranged entry.
NAMETABLE_SUP_INST0 = """\
Type       Lower      Upper      Scope    Port       Node
2147614721 0          0          node     1321986434
2147549344 0          0          node     2557553135
2147549344 65367      65367      cluster  554201909
"""

# Supervisor bound at instance 3 (a machine_index=3 rig).
NAMETABLE_SUP_INST3 = """\
Type       Lower      Upper      Scope    Port       Node
2147614721 3          3          node     111
"""

# A ranged publication that COVERS instance 5 (Lower 4 <= 5 <= Upper 6).
NAMETABLE_RANGE = """\
Type       Lower      Upper      Scope    Port       Node
2147614721 4          6          cluster  222
"""

# No supervisor at all (only some other service).
NAMETABLE_NO_SUP = """\
Type       Lower      Upper      Scope    Port       Node
2147549344 0          0          node     999
"""


def _mock_tipc(stdout):
    """Patch subprocess.run so _tipc_addr_bound sees `stdout` as the nametable."""
    return mock.patch.object(
        theia.subprocess, "run",
        return_value=mock.Mock(stdout=stdout))


def test_bound_at_instance_zero():
    with _mock_tipc(NAMETABLE_SUP_INST0):
        assert theia._tipc_addr_bound(SUP_TYPE, 0) is True
        # a DIFFERENT instance of the same type is NOT bound
        assert theia._tipc_addr_bound(SUP_TYPE, 1) is False


def test_bound_at_nonzero_instance():
    with _mock_tipc(NAMETABLE_SUP_INST3):
        assert theia._tipc_addr_bound(SUP_TYPE, 3) is True
        assert theia._tipc_addr_bound(SUP_TYPE, 0) is False


def test_ranged_publication_covers_instance():
    # Lower..Upper is inclusive: 4,5,6 bound; 3 and 7 not.
    with _mock_tipc(NAMETABLE_RANGE):
        for inst in (4, 5, 6):
            assert theia._tipc_addr_bound(SUP_TYPE, inst) is True
        assert theia._tipc_addr_bound(SUP_TYPE, 3) is False
        assert theia._tipc_addr_bound(SUP_TYPE, 7) is False


def test_not_bound_when_supervisor_absent():
    with _mock_tipc(NAMETABLE_NO_SUP):
        assert theia._tipc_addr_bound(SUP_TYPE, 0) is False


def test_header_and_blank_rows_ignored():
    junk = "Type Lower Upper\n\n   \nnot-a-number row here\n" + NAMETABLE_SUP_INST0
    with _mock_tipc(junk):
        assert theia._tipc_addr_bound(SUP_TYPE, 0) is True


def test_missing_tipc_tool_returns_false():
    # If `tipc` isn't installed, don't block a start on the tooling gap.
    with mock.patch.object(theia.subprocess, "run",
                           side_effect=FileNotFoundError):
        assert theia._tipc_addr_bound(SUP_TYPE, 0) is False


def test_tipc_subprocess_error_returns_false():
    with mock.patch.object(theia.subprocess, "run",
                           side_effect=theia.subprocess.SubprocessError):
        assert theia._tipc_addr_bound(SUP_TYPE, 0) is False


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))

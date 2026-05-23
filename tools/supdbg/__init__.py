# supdbg — text-mode supervisor debugger.
#
# Modeled (loosely) on Erlang/OTP's `dbg` module: a thin client over
# the supervisor's existing gRPC control + observation surface
# (services/com SupervisorView). Provides:
#
#   - supdbg.client.Client         programmatic API (use from tests)
#   - supdbg.repl                  interactive REPL  (`python -m supdbg`)
#   - supdbg.cli                   one-shot subcommands (argparse)
#
# Unlike dbg (which speaks BIF-level `trace:`), supdbg works on
# our supervisor's vocabulary: tree, start_child, restart, terminate,
# delete, watch (events/health/snapshot stream).

from .client import Client, Observation, EventKind, ChildStateView  # noqa: F401

__all__ = ["Client", "Observation", "EventKind", "ChildStateView"]

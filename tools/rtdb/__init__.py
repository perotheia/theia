# rtdb — the Remote Theia Debug Bridge (tdb over gRPC).
#
# rtdb is tdb's twin for REMOTE operation: the SAME verbs + render layer
# (tools/tdb/tdb_commands), but driven over gRPC to services/com's
# SupervisorView instead of local TIPC. com is the gRPC↔Theia proxy that lets
# an operator outside the DMZ observe + control the system over IP.
#
# Entry points (flat scripts, like tdb — not a package API):
#   rtdb.py         CLI + REPL (symlinked to .venv/bin/rtdb by env.sh)
#   rtdb_client.py  gRPC SupervisorClient/TraceClient mirroring tdb_client's
#                   surface, so the shared cmd_* run unchanged over gRPC.
#
# Generate the gRPC stubs with tools/rtdb/gen_protos.sh (output: _gen/).

#!/usr/bin/env bash
# tools/rtdb/gen_protos.sh — regenerate rtdb's Python gRPC stubs.
#
# rtdb is "tdb over gRPC": it drives services/com's SupervisorView (control +
# the GetTree-poll live tree) + the collector's TraceStream (logcat). This
# script invokes grpc_tools.protoc (from the workspace .venv) against the
# CONSOLIDATED protos:
#   - services/com/proto/supervisor_bridge.proto  (the gRPC service)
#   - platform/proto/system/supervisor/supervisor.proto  (system_supervisor.*)
#   - platform/runtime/proto/platform_runtime/runtime.proto  (LogLevelValue etc.)
#   - services/log/proto/trace_stream.proto       (TraceStream egress, if present)
#
# Output lands in tools/rtdb/_gen/ as *_pb2.py + *_pb2_grpc.py. The runtime
# proto keeps its platform_runtime/ path prefix so supervisor.proto's
# `import "platform_runtime/runtime.proto"` resolves. Idempotent.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "$SCRIPT_DIR/../.." && pwd)"
VENV="$WORKSPACE/.venv"

if [[ ! -x "$VENV/bin/python" ]]; then
    echo "ERROR: workspace venv not found at $VENV — run 'python -m venv .venv && pip install -e artheia/'" >&2
    exit 1
fi

PROTO_COM="$WORKSPACE/services/com/proto"
PROTO_SUP="$WORKSPACE/platform/proto/system/supervisor"
PROTO_RT="$WORKSPACE/platform/runtime/proto"          # platform_runtime/runtime.proto
PROTO_LOG="$WORKSPACE/services/log/proto"
OUT_DIR="$SCRIPT_DIR/_gen"

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

# All proto roots on the path so the cross-imports resolve:
#   supervisor_bridge.proto  → import "supervisor.proto"
#   supervisor.proto         → import "platform_runtime/runtime.proto"
#   trace_stream.proto       → import "supervisor_bridge.proto"
# grpc_tools rewrites `import "Foo.proto"` → `import Foo_pb2` relative to
# --python_out, so the flat _gen/ dir + sys.path insertion (rtdb_client does
# it) makes the imports resolve.
PROTOS=(
    "$PROTO_COM"/supervisor_bridge.proto
    "$PROTO_SUP"/supervisor.proto
    "$PROTO_RT"/platform_runtime/runtime.proto
)
# trace_stream.proto is optional (the trace EGRESS gRPC; moves into com in a
# later phase). Include it when present so `rtdb logcat` has its stubs.
if [[ -f "$PROTO_LOG/trace_stream.proto" ]]; then
    PROTOS+=("$PROTO_LOG"/trace_stream.proto)
fi

"$VENV/bin/python" -m grpc_tools.protoc \
    --proto_path="$PROTO_COM" \
    --proto_path="$PROTO_SUP" \
    --proto_path="$PROTO_RT" \
    --proto_path="$PROTO_LOG" \
    --python_out="$OUT_DIR" \
    --grpc_python_out="$OUT_DIR" \
    "${PROTOS[@]}"

# Importable as a package + flat-style imports resolve via sys.path (rtdb_client
# inserts _gen/ before importing).
touch "$OUT_DIR/__init__.py"

echo "rtdb: regenerated $(ls "$OUT_DIR"/*.py | wc -l) Python modules in $OUT_DIR"

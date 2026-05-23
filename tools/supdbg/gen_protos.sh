#!/usr/bin/env bash
# tools/supdbg/gen_protos.sh — regenerate Python gRPC stubs.
#
# Invokes grpc_tools.protoc (from the workspace .venv) against
# services/com/proto/supervisor_bridge.proto and the supervisor's
# message-type .protos. Output lands in tools/supdbg/_gen/ as
# *_pb2.py (messages) and *_pb2_grpc.py (service stubs).
#
# Run after editing any .art schema that touches the supervisor
# message types (since artheia regenerates the .protos from .art).
#
# Idempotent — safe to re-run; just overwrites the _gen/ files.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "$SCRIPT_DIR/../.." && pwd)"
VENV="$WORKSPACE/.venv"

if [[ ! -x "$VENV/bin/python" ]]; then
    echo "ERROR: workspace venv not found at $VENV — run 'python -m venv .venv && pip install -e artheia/'" >&2
    exit 1
fi

PROTO_SUP="$WORKSPACE/platform/supervisor/generated/proto"
PROTO_COM="$WORKSPACE/services/com/proto"
OUT_DIR="$SCRIPT_DIR/_gen"

mkdir -p "$OUT_DIR"

# grpc_tools.protoc rewrites imports of `Foo.proto` → `import Foo_pb2`
# *relative to --python_out*. Both proto trees must be on the path so
# `import "ChildState.proto"` from supervisor_bridge.proto resolves.
"$VENV/bin/python" -m grpc_tools.protoc \
    --proto_path="$PROTO_SUP" \
    --proto_path="$PROTO_COM" \
    --python_out="$OUT_DIR" \
    --grpc_python_out="$OUT_DIR" \
    "$PROTO_COM"/supervisor_bridge.proto \
    "$PROTO_SUP"/*.proto

# Make the generated package importable as supdbg._gen — drop an
# __init__.py so `import supdbg._gen.supervisor_bridge_pb2` works.
touch "$OUT_DIR/__init__.py"

# grpc_tools emits flat-style imports (`import ChildState_pb2`) — the
# files live in the same dir so this works as long as we add _gen/ to
# sys.path. supdbg.client.py does that before importing.

echo "supdbg: regenerated $(ls "$OUT_DIR"/*.py | wc -l) Python modules in $OUT_DIR"

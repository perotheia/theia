#!/usr/bin/env bash
# theia MCP launcher — workspace-relative, no hardcoded paths.
#
# Exposes the `theia` dev-loop CLI + the tdb/rtdb live-inspect bridges + the
# colony web-API deploy tools as MCP tools (tools/theia_mcp.py). Mirrors
# artheia/run_mcp.sh: uses the single workspace .venv, pointed at by the
# workspace-root .mcp.json so Claude Code discovers it automatically.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"   # tools/
WORKSPACE="$(cd "${SCRIPT_DIR}/.." && pwd)"                       # repo / ws root

VENV_PY="${WORKSPACE}/.venv/bin/python"
if [[ ! -x "${VENV_PY}" ]]; then
    echo "theia-mcp: ${VENV_PY} missing — create the workspace venv:" >&2
    echo "  python3 -m venv .venv && ./.venv/bin/pip install fastmcp httpx" >&2
    exit 1
fi

# Resolve relative paths the model passes (rig targets, .art files) against the
# dir the user is working in (the workspace root), not tools/.
export THEIA_INVOCATION_CWD="${THEIA_INVOCATION_CWD:-$WORKSPACE}"

# theia.py + tdb/rtdb import runtime helpers from the workspace tree; put tools/
# and the repo root on PYTHONPATH so `import theia` resolves to tools/theia.py.
export PYTHONPATH="${SCRIPT_DIR}:${WORKSPACE}:${PYTHONPATH:-}"

# colony web API — the deploy edge. Override per-site; defaults to the dalek
# fleet host. COLONY_API_KEY (if the server enforces it) is read from the env.
export COLONY_API_URL="${COLONY_API_URL:-http://dalek:8081}"

exec "${VENV_PY}" "${SCRIPT_DIR}/theia_mcp.py" "$@"

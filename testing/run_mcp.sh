#!/usr/bin/env bash
# rf-theia MCP launcher — workspace-relative, no hardcoded paths.
#
# Activates the framework's own .venv (testing/.venv) and exec's the
# MCP server. Pointed at by workspace-root .mcp.json so Claude Code
# discovers it automatically.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TESTING="${SCRIPT_DIR}"
WORKSPACE="$(cd "${TESTING}/.." && pwd)"

VENV_PY="${TESTING}/.venv/bin/python"
if [[ ! -x "${VENV_PY}" ]]; then
    echo "rf-theia: ${VENV_PY} missing — run:" >&2
    echo "  cd ${TESTING} && python3 -m venv .venv && \\" >&2
    echo "    ./.venv/bin/pip install -r requirements.txt" >&2
    exit 1
fi

# rf_theia package lives under testing/. Adapters import tools.supdbg
# from the workspace root, so both go on PYTHONPATH.
export PYTHONPATH="${TESTING}:${WORKSPACE}:${PYTHONPATH:-}"

exec "${VENV_PY}" -m rf_theia.adapters.mcp_server "$@"

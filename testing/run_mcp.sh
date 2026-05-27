#!/usr/bin/env bash
# rf-theia MCP launcher — workspace-relative, no hardcoded paths.
#
# Uses the single workspace .venv (rf-theia is editable-installed there via
# `pip install -e testing/[mcp,dev]`). Pointed at by workspace-root .mcp.json
# so Claude Code discovers it automatically.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TESTING="${SCRIPT_DIR}"
WORKSPACE="$(cd "${TESTING}/.." && pwd)"

VENV_PY="${WORKSPACE}/.venv/bin/python"
if [[ ! -x "${VENV_PY}" ]]; then
    echo "rf-theia: ${VENV_PY} missing — run:" >&2
    echo "  python3 -m venv .venv && \\" >&2
    echo "    ./.venv/bin/pip install -e 'testing/[mcp,dev]'" >&2
    exit 1
fi

# rf_theia is installed in the venv; adapters also import tools.supdbg from
# the workspace root, so put the workspace on PYTHONPATH.
export PYTHONPATH="${WORKSPACE}:${PYTHONPATH:-}"

exec "${VENV_PY}" -m rf_theia.adapters.mcp_server "$@"

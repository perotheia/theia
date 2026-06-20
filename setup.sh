#!/usr/bin/env bash
# Theia environment — source this to put a Theia checkout on your PATH, the
# catkin `devel/setup.bash` / ROS `/opt/ros/<distro>/setup.bash` analogue.
#
#   source /path/to/theia/setup.sh
#
# Exports THEIA_ROOT (this checkout) + prepends its venv/bin and the `theia`
# launcher to PATH, and puts artheia + the workspace Python layer on
# PYTHONPATH. A CONSUMING workspace (e.g. gataway_ws) sources the SIBLING
# theia's setup.sh, then runs `theia init` to scaffold itself against it.
#
# Works both for an in-repo checkout (sibling-source mode, PSP/gateway still in
# flight) and an installed prefix (/opt/theia) once Theia ships as a deb.

# Resolve this script's dir even when sourced (bash + zsh).
if [ -n "${BASH_SOURCE:-}" ]; then _theia_self="${BASH_SOURCE[0]}";
elif [ -n "${ZSH_VERSION:-}" ]; then _theia_self="${(%):-%N}";
else _theia_self="$0"; fi
THEIA_ROOT="$(cd "$(dirname "$_theia_self")" && pwd)"
export THEIA_ROOT
unset _theia_self

# venv (editable artheia + workspace layer) first on PATH.
if [ -d "$THEIA_ROOT/.venv/bin" ]; then
  case ":$PATH:" in *":$THEIA_ROOT/.venv/bin:"*) ;; *) PATH="$THEIA_ROOT/.venv/bin:$PATH";; esac
fi
# The `theia` launcher (theia.py wrapper) — add the repo root so `theia` resolves.
case ":$PATH:" in *":$THEIA_ROOT:"*) ;; *) PATH="$THEIA_ROOT:$PATH";; esac
export PATH

# artheia (the DSL/generator package) + the workspace Python layer on PYTHONPATH
# for consumers that don't pip-install it.
case ":${PYTHONPATH:-}:" in
  *":$THEIA_ROOT/artheia:"*) ;;
  *) PYTHONPATH="$THEIA_ROOT/artheia${PYTHONPATH:+:$PYTHONPATH}";;
esac
export PYTHONPATH

# THEIA_WORKSPACE — the dir this setup.sh was SOURCED FROM (the consuming
# workspace, e.g. demo/ or gataway_ws), when it differs from THEIA_ROOT (the
# framework checkout). A consuming workspace sources the SIBLING theia's
# setup.sh from its own root, so $PWD at source time IS the workspace. When you
# source the framework's own setup.sh from the framework root, the two match and
# THEIA_WORKSPACE is left unset.
_theia_pwd="$(pwd)"
if [ "$_theia_pwd" != "$THEIA_ROOT" ]; then
  THEIA_WORKSPACE="$_theia_pwd"
  export THEIA_WORKSPACE
fi

# THEIA_TRACE_DECODER_PATH — colon-separated DIRS the pluggable trace decoder
# scans for `libtrace_decoder_*.so` (framework system plugin + the workspace's
# app plugin + an installed prefix). Consumers (supervisor-gui, rf-theia
# adapter, rtdb) dlopen EVERY plugin found here and try each to decode a record.
_theia_decoder_dirs="$THEIA_ROOT/bazel-bin/platform/runtime/trace"
if [ -n "${THEIA_WORKSPACE:-}" ]; then
  _theia_decoder_dirs="$_theia_decoder_dirs:$THEIA_WORKSPACE/bazel-bin/trace"
fi
if [ -d /opt/theia/lib ]; then
  _theia_decoder_dirs="$_theia_decoder_dirs:/opt/theia/lib"
fi
case ":${THEIA_TRACE_DECODER_PATH:-}:" in
  *":$_theia_decoder_dirs:"*) ;;
  *) THEIA_TRACE_DECODER_PATH="$_theia_decoder_dirs${THEIA_TRACE_DECODER_PATH:+:$THEIA_TRACE_DECODER_PATH}";;
esac
export THEIA_TRACE_DECODER_PATH
unset _theia_pwd _theia_decoder_dirs

echo "theia: THEIA_ROOT=$THEIA_ROOT (sourced)"

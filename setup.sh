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

echo "theia: THEIA_ROOT=$THEIA_ROOT (sourced)"

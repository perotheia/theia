# env.sh — source me to set up the Theia dev shell (zsh).
#
#   source env.sh
#
# Activates the workspace .venv (puts `artheia`, `theia`, `bazel`
# wrappers on PATH) and registers tab completion for `artheia` and
# `theia`. zsh is the supported shell; bash is handled as a fallback.
#
# Completion is click's native shell-source (see
# docs/artheia/completion.md) — no argcomplete / extra deps. For zsh it
# needs the completion system initialised (`compinit`), which this
# script ensures before evaluating the click snippets.
#
# Idempotent: re-sourcing is safe (compinit is only run once per shell).

# --- locate this script's directory (works under bash and zsh) ----------
# zsh: ${(%):-%x} is the path of the sourced file. bash: ${BASH_SOURCE[0]}.
if [ -n "${ZSH_VERSION:-}" ]; then
    _THEIA_ENV_SRC="${(%):-%x}"
else
    _THEIA_ENV_SRC="${BASH_SOURCE[0]}"
fi
_THEIA_ROOT="$(cd "$(dirname "$_THEIA_ENV_SRC")" && pwd)"

# --- activate the venv ---------------------------------------------------
if [ -f "$_THEIA_ROOT/.venv/bin/activate" ]; then
    # shellcheck disable=SC1091
    . "$_THEIA_ROOT/.venv/bin/activate"
else
    echo "env.sh: no .venv at $_THEIA_ROOT/.venv — create it first " \
         "(python -m venv .venv && pip install -e artheia)" >&2
    return 1 2>/dev/null || exit 1
fi

# --- shell completion ----------------------------------------------------
if [ -n "${ZSH_VERSION:-}" ]; then
    # zsh: load the completion system once, then source click's snippets.
    # `compinit` defines `compdef`, which the click zsh_source output calls.
    if ! whence compdef >/dev/null 2>&1; then
        autoload -Uz compinit && compinit -u
    fi
    eval "$(_ARTHEIA_COMPLETE=zsh_source artheia)"
    eval "$(_THEIA_COMPLETE=zsh_source theia)"
elif [ -n "${BASH_VERSION:-}" ]; then
    # bash fallback (you're on zsh per the project default, but keep this
    # so sourcing from a bash subshell still works).
    eval "$(_ARTHEIA_COMPLETE=bash_source artheia)"
    eval "$(_THEIA_COMPLETE=bash_source theia)"
fi

echo "theia env ready: .venv active, completion for artheia + theia loaded"

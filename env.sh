# env.sh — source me to set up the Theia dev shell (zsh).
#
#   source env.sh
#
# Activates the workspace .venv (puts `artheia`, `theia`, `tdb`, `bazel`
# wrappers on PATH), ensures the workspace-CLI symlinks (`theia`, `tdb`,
# `rtdb`) exist in .venv/bin, and registers tab completion for `artheia`,
# `theia`, `tdb`, and `rtdb`. zsh is the supported shell; bash is a fallback.
#
# Completion sources: artheia is click (its native shell-source, see
# docs/artheia/completion.md); theia/tdb/rtdb each expose a hidden
# `__complete` that prints their live verb map, so the lists never drift from
# the code (with a static fallback so a tool that can't import — e.g. rtdb
# before its protos are built — still completes). For zsh it needs the
# completion system initialised (`compinit`), which this script ensures.
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

# THEIA_ROOT — the workspace root, exported so in-source tools resolve paths
# without $PWD assumptions (rtdb defaults its dev-cert dir to
# $THEIA_ROOT/dist/manifest/<machine>/certs; theia/tdb use it too).
export THEIA_ROOT="$_THEIA_ROOT"

# --- activate the venv ---------------------------------------------------
if [ -f "$_THEIA_ROOT/.venv/bin/activate" ]; then
    # shellcheck disable=SC1091
    . "$_THEIA_ROOT/.venv/bin/activate"
else
    echo "env.sh: no .venv at $_THEIA_ROOT/.venv — create it first " \
         "(python -m venv .venv && pip install -e artheia)" >&2
    return 1 2>/dev/null || exit 1
fi

# --- workspace-CLI symlinks (idempotent) ---------------------------------
# `theia` (workspace dispatcher), `tdb` (local TIPC debug bridge) and `rtdb`
# (the SAME bridge over gRPC to com, for remote/out-of-DMZ operation) are plain
# script entrypoints, not pip console_scripts — link them into .venv/bin so
# they're on PATH alongside `artheia`. `ln -sf` makes re-sourcing a no-op.
# (The .venv is gitignored, so this is where the symlinks get (re)created.)
ln -sf "$_THEIA_ROOT/theia"              "$_THEIA_ROOT/.venv/bin/theia"
ln -sf "$_THEIA_ROOT/tools/tdb/tdb.py"   "$_THEIA_ROOT/.venv/bin/tdb"
ln -sf "$_THEIA_ROOT/tools/rtdb/rtdb.py" "$_THEIA_ROOT/.venv/bin/rtdb"
chmod +x "$_THEIA_ROOT/theia" "$_THEIA_ROOT/tools/theia.py" \
         "$_THEIA_ROOT/tools/tdb/tdb.py" "$_THEIA_ROOT/tools/rtdb/rtdb.py" 2>/dev/null

# --- shell completion ----------------------------------------------------
# theia / tdb / rtdb are plain dispatch CLIs (no click). Rather than hardcode
# their verb sets here — which silently drifts as commands are added/renamed —
# each exposes a hidden `__complete` that prints its verb list straight from its
# own COMMANDS map. We pull the live list at source time (with a static fallback
# so completion never breaks the shell if a tool can't import, e.g. rtdb before
# its protos are built). artheia is click-based and ships native completion.
_theia_verbs() { theia __complete 2>/dev/null || echo "init rig provision orchestrate install start stop manifest dist release compdb"; }
_tdb_verbs()   { tdb   __complete 2>/dev/null || echo "apps ps supervisor info trace trace-config loglevel restart terminate logcat get-snapshot help quit"; }
_rtdb_verbs()  { rtdb  __complete 2>/dev/null || echo "apps ps supervisor info trace trace-config loglevel restart terminate logcat schemas snapshot help quit"; }

if [ -n "${ZSH_VERSION:-}" ]; then
    # zsh: load the completion system once, then source click's snippets.
    # `compinit` defines `compdef`, which the click zsh_source output calls.
    if ! whence compdef >/dev/null 2>&1; then
        autoload -Uz compinit && compinit -u
    fi
    # artheia is click-based — use its native completion source.
    eval "$(_ARTHEIA_COMPLETE=zsh_source artheia)"
    # The dispatch CLIs: complete from each tool's live __complete verb list.
    _theia_complete() { compadd ${(z)"$(_theia_verbs)"} }
    compdef _theia_complete theia
    _tdb_complete() { compadd ${(z)"$(_tdb_verbs)"} }
    compdef _tdb_complete tdb
    _rtdb_complete() { compadd ${(z)"$(_rtdb_verbs)"} }
    compdef _rtdb_complete rtdb
elif [ -n "${BASH_VERSION:-}" ]; then
    # bash fallback (you're on zsh per the project default, but keep this
    # so sourcing from a bash subshell still works).
    eval "$(_ARTHEIA_COMPLETE=bash_source artheia)"
    complete -W "$(_theia_verbs)" theia
    complete -W "$(_tdb_verbs)" tdb
    complete -W "$(_rtdb_verbs)" rtdb
fi

echo "theia env ready: .venv active; theia + tdb linked; completion for artheia + theia + tdb + rtdb"

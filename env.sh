# env.sh — the SOURCE-side Theia activation (the catkin `devel/setup.bash` /
# ROS `/opt/ros/<distro>/setup.bash` analogue for a source checkout). The
# installed deb ships its own /opt/theia/setup.{bash,zsh}; in source you use
# THIS. Two ways to source it:
#
#   source env.sh                 # from the framework root — dev the framework
#   cd my_ws && source ../theia/env.sh   # from a CONSUMING workspace — activate
#                                          a sibling framework checkout (sets
#                                          THEIA_WORKSPACE to the cwd)
#
# Activates the framework .venv (puts `artheia`, `theia`, `tdb`, `bazel`
# wrappers on PATH), ensures the workspace-CLI symlinks (`theia`, `tdb`,
# `rtdb`) exist in .venv/bin, exports THEIA_ROOT / THEIA_WORKSPACE /
# THEIA_TRACE_DECODER_PATH, and registers tab completion for `artheia`,
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

# THEIA_ROOT — the framework checkout (where THIS env.sh lives), exported so
# in-source tools resolve paths without $PWD assumptions (rtdb defaults its
# dev-cert dir to $THEIA_ROOT/dist/manifest/<machine>/certs; theia/tdb use it).
export THEIA_ROOT="$_THEIA_ROOT"

# THEIA_WORKSPACE — the dir env.sh was SOURCED FROM, when it differs from the
# framework checkout. This is the source-side analogue of the deb's
# /opt/theia/setup.sh: a CONSUMING workspace (demo/, gataway_ws) cd's into its
# own root and `source ../theia/env.sh`, so $PWD at source time IS the
# workspace. theia/tdb/rtdb read the rig + dist/manifest + install/ from HERE,
# not from the framework. Sourcing env.sh from the framework root leaves the two
# equal and THEIA_WORKSPACE unset.
_theia_pwd="$(pwd)"
if [ "$_theia_pwd" != "$_THEIA_ROOT" ]; then
    export THEIA_WORKSPACE="$_theia_pwd"
fi

# --- activate the venv ---------------------------------------------------
# The framework's venv (editable artheia + the tool console scripts). A
# consuming workspace has no venv of its own — it borrows the framework's.
if [ -f "$_THEIA_ROOT/.venv/bin/activate" ]; then
    # shellcheck disable=SC1091
    . "$_THEIA_ROOT/.venv/bin/activate"
else
    echo "env.sh: no .venv at $_THEIA_ROOT/.venv — create it first " \
         "(python -m venv .venv && pip install -e artheia)" >&2
    return 1 2>/dev/null || exit 1
fi

# THEIA_TRACE_DECODER_PATH — colon-separated DIRS the pluggable trace decoder
# scans for `libtrace_decoder_*.so` (framework system plugin + the workspace's
# app plugin + an installed prefix). Consumers (supervisor-gui, rf-theia, rtdb)
# dlopen every plugin found here and try each to decode a record.
_theia_decoder_dirs="$_THEIA_ROOT/bazel-bin/platform/runtime/trace"
if [ -n "${THEIA_WORKSPACE:-}" ]; then
    _theia_decoder_dirs="$_theia_decoder_dirs:$THEIA_WORKSPACE/bazel-bin/trace"
fi
if [ -d /opt/theia/lib ]; then
    _theia_decoder_dirs="$_theia_decoder_dirs:/opt/theia/lib"
fi
case ":${THEIA_TRACE_DECODER_PATH:-}:" in
    *":$_theia_decoder_dirs:"*) ;;
    *) export THEIA_TRACE_DECODER_PATH="$_theia_decoder_dirs${THEIA_TRACE_DECODER_PATH:+:$THEIA_TRACE_DECODER_PATH}";;
esac
unset _theia_pwd _theia_decoder_dirs

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

# --- rtdb gRPC stubs (idempotent, regen only when the proto is newer) -----
# rtdb's _gen/*_pb2.py are generated from services/com/proto/supervisor_bridge.
# proto and GITIGNORED (per-checkout). If the proto gains an rpc (e.g. NmView.
# SetVpn) the stale stub raises AttributeError at call time. Regenerate when the
# proto is newer than the stub (or the stub is missing) — keeps them in lockstep
# the same way the symlinks above stay current. Cheap no-op when up to date.
_rtdb_proto="$_THEIA_ROOT/services/com/proto/supervisor_bridge.proto"
_rtdb_stub="$_THEIA_ROOT/tools/rtdb/_gen/supervisor_bridge_pb2_grpc.py"
if [ -f "$_rtdb_proto" ] && \
   { [ ! -f "$_rtdb_stub" ] || [ "$_rtdb_proto" -nt "$_rtdb_stub" ]; }; then
    bash "$_THEIA_ROOT/tools/rtdb/gen_protos.sh" >/dev/null 2>&1 \
        && echo "env.sh: regenerated rtdb gRPC stubs (proto changed)" \
        || echo "env.sh: rtdb stub regen failed — run tools/rtdb/gen_protos.sh" >&2
fi
unset _rtdb_proto _rtdb_stub

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
    # NOTE: these bodies use zsh-only syntax (compadd, ${(z)...}). They're inside
    # `eval` so a BASH parser (e.g. setup_local.sh sourced from bash) never tries
    # to PARSE them — bash parses every branch of an if/elif even when not taken,
    # and the bare zsh forms below were a parse error that aborted the whole file.
    eval '_theia_complete() { compadd ${(z)"$(_theia_verbs)"} }; compdef _theia_complete theia'
    eval '_tdb_complete()   { compadd ${(z)"$(_tdb_verbs)"} };   compdef _tdb_complete tdb'
    eval '_rtdb_complete()  { compadd ${(z)"$(_rtdb_verbs)"} };  compdef _rtdb_complete rtdb'
elif [ -n "${BASH_VERSION:-}" ]; then
    # bash fallback (you're on zsh per the project default, but keep this
    # so sourcing from a bash subshell still works).
    eval "$(_ARTHEIA_COMPLETE=bash_source artheia)"
    complete -W "$(_theia_verbs)" theia
    complete -W "$(_tdb_verbs)" tdb
    complete -W "$(_rtdb_verbs)" rtdb
fi

echo "theia env ready: .venv active; theia + tdb linked; completion for artheia + theia + tdb + rtdb"

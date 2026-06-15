#!/usr/bin/env bash
# statusline.sh — render the work-with-me indicator for Claude Code's statusLine.
#
# Reads the project's status sidecar (.claude/work-with-me/state.json), which
# the MCP server keeps up to date with the post-window-filter counts. Output
# is a single short line; non-zero exit makes the statusline blank, so we
# always exit 0 even on missing state (just prints nothing).
#
# Wire it in .claude/settings.json (local/gitignored — non-intrusive):
#   "statusLine": {
#     "type": "command",
#     "command": ".claude/plugins/work-with-me/statusline.sh",
#     "refreshInterval": 5
#   }
#
# Format:                  Examples:
#   me:<files>/<cmds>         me:5/3        — 5 files, 3 commands tracked
#   me:off                    me:off        — watcher is off
#   *me:<files>/<cmds>        *me:5/3       — focus is set (prefix marker)
#
# `files` and `cmds` are the same numbers `check me` would surface — files
# is distinct-paths-touched, cmds is shell commands AFTER the time-window
# filter against those file events (commands from other terminals are
# already excluded). Stays compact: short prefix, no padding, no ANSI by
# default (terminals + light themes vary; users style their own statusline).

set -u

# Read the Claude Code statusLine JSON payload off stdin — we only need
# the project dir, but consuming stdin is required so the parent isn't
# blocked on a pipe write.
payload="$(cat)"
project="$(printf '%s' "$payload" | jq -r '.workspace.project_dir // .cwd // empty' 2>/dev/null)"
project="${project:-${CLAUDE_PROJECT_DIR:-$PWD}}"

state="${project}/.claude/work-with-me/state.json"
[ -r "$state" ] || exit 0       # server not running → blank (not an error)

enabled="$(jq -r '.enabled // false'  "$state" 2>/dev/null)"
files="$(  jq -r '.files   // 0'      "$state" 2>/dev/null)"
cmds="$(   jq -r '.cmds    // 0'      "$state" 2>/dev/null)"
focus="$(  jq -r '.focus   // ""'     "$state" 2>/dev/null)"

if [ "$enabled" != "true" ]; then
    printf 'me:off'
    exit 0
fi

prefix=""
[ -n "$focus" ] && prefix="*"   # marker that a focus is set; don't dump the text

printf '%sme:%s/%s' "$prefix" "$files" "$cmds"
exit 0

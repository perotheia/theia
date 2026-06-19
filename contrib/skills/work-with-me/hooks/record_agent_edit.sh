#!/usr/bin/env bash
# record_agent_edit.sh — PostToolUse hook for the work-with-me plugin.
#
# Fires after Claude's own Edit/Write/MultiEdit/NotebookEdit tools. Records
# the edited file's ABSOLUTE path to the plugin's sidecar JSONL so the
# watcher can suppress the matching inotify event and attribute it to the
# agent (not the user). Wire it in .claude/settings.json:
#
#   "hooks": { "PostToolUse": [ {
#       "matcher": "Edit|Write|MultiEdit|NotebookEdit",
#       "hooks": [ { "type": "command",
#                    "command": "${CLAUDE_PROJECT_DIR}/contrib/skills/work-with-me/hooks/record_agent_edit.sh" } ]
#   } ] }
#
# Reads the hook payload (JSON) on stdin; needs `jq`. Always exits 0 so it
# never blocks a tool call.
set -euo pipefail

ROOT="${CLAUDE_PROJECT_DIR:-$(pwd)}"
SIDECAR="$ROOT/.claude/work-with-me/agent-edits.jsonl"
mkdir -p "$(dirname "$SIDECAR")"

PAYLOAD="$(cat)"

# Extract the edited path. Edit/Write/MultiEdit use tool_input.file_path;
# NotebookEdit uses tool_input.notebook_path. Take whichever is present.
FILE_PATH="$(printf '%s' "$PAYLOAD" | jq -r '
    .tool_input.file_path // .tool_input.notebook_path // empty')" || FILE_PATH=""
[ -z "$FILE_PATH" ] && exit 0

# Normalise to absolute (watchdog reports absolute paths). A relative
# file_path is resolved against the project root.
case "$FILE_PATH" in
    /*) ABS="$FILE_PATH" ;;
    *)  ABS="$ROOT/$FILE_PATH" ;;
esac

TS_NS="$(date -u +%s%N)"
printf '{"ts_ns": %s, "path": %s}\n' "$TS_NS" "$(printf '%s' "$ABS" | jq -R .)" >> "$SIDECAR"
exit 0

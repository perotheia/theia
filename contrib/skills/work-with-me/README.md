# work-with-me

A Claude Code MCP plugin that watches **your** (the human's) file changes
and lets you ask Claude to review them for consistency at any point during a
session.

The plugin source is tracked under `contrib/skills/work-with-me/`; Claude Code
discovers it through the `work-with-me` entry in the repo-root `.mcp.json`, which
points the server directly at that path. It runs on the **workspace `.venv`** —
no per-plugin venv.

## How it works

- Starts an **inotify watcher** (via `watchdog`) when Claude Code launches
  the server; covers the full repo tree recursively.
- Respects `.gitignore` (and always skips `.git/` and `.claude/`); reloads
  patterns whenever `.gitignore` itself changes.
- **Attributes changes to the user, not the agent** (see below) — Claude's
  own `Edit`/`Write` edits are excluded so `check me` reviews only *your*
  manual changes.
- Keeps a **running in-memory log** of `created / modified / deleted / moved`
  events, deduplicating burst saves.
- **Tails your shell history** (`$HISTFILE`, default `~/.zsh_history`) — zsh's
  history is append-only and shared across terminals, so this is a single
  chronological feed of what you ran (build, test, git, …). `check_me` pairs
  it with the file edits as **intent signal**: it can cross-check "you edited
  X" against "you ran Y". Read-only; baseline starts when the watcher does,
  so only post-start commands are reported. Capped at the most recent
  `SHELL_MAX` (200) per checkpoint.
  - **Time-correlated to this repo's activity.** Because the history is
    shared, `check_me` keeps only commands near a file-change event: a
    command at `Tc` is kept iff some edit `Tf` falls in
    `[Tc - SHELL_PRE_S, Tc + SHELL_POST_S]` (default 3 s / 20 s). The window
    is asymmetric — `PRE` is small (you ran something, then edited); `POST`
    is wider because a file-writing command (generator, compiler, `cp`,
    `git`) lands its outputs *seconds after* it runs, so the edits sit in the
    command's near future. Commands with no nearby edit — i.e. from another
    terminal doing unrelated work — are dropped. Fails open: with no edits to
    anchor against, or a history line with no timestamp, the command is kept.

| Tool | Slash | Trigger phrase | What it does |
|---|---|---|---|
| `watch_me` | `/watch-me` | *"watch me"* | Start tracking (turn the watcher **on**, fresh checkpoint). **Off by default** so the plugin is non-intrusive for teammates sharing `.mcp.json`. If already on, reports buffered counts. |
| `check_me` | `/check-me` | *"check me"* | Review file edits + correlated shell commands; gives advice; **clears checkpoint**. |
| `fix_me` | `/fix-me` | *"fix me"* | Same review surface as `check_me`, but the agent ALSO applies the fixes (Edit/Write), not just reports. Use when you know the recent edits broke something. **Clears checkpoint**. |
| `compare_me` | `/compare-me` | *"compare me" / "diff me"* | Like `check_me` but includes the actual `git diff` of touched files — review sees content, not just paths. **Clears checkpoint**. |
| `focus_me` | `/focus-me <goal>` | *"focus me on X" / "clear focus"* | Set the one-line session goal; the next `check_me` / `compare_me` uses it as explicit intent. Pass empty to clear. |
| `ignore_me` | `/ignore-me` | *"ignore me"* | Stop tracking + discard everything buffered since the last `watch me` — **no review**, watcher off (aborted attempt). |
| `undo_me` | `/undo-me` | *"undo me"* | Show diff + `git checkout --` revert hints for touched files. **Non-destructive** (does NOT run any revert); **does NOT clear** the checkpoint. |
| `list_me` | `/list-me` | *"list me" / "show me"* | Passive listing of the current buffer (file events + correlated shell commands). **No analysis, no diff, no clear.** Glance before picking `check_me` / `compare_me` / `undo_me` / `ignore_me`. |

`watch me` and `ignore me` are the two on/off verbs; the others operate on
the current session. Typical loop: `watch me` → work → `check me` → repeat.
`focus me on X` once at the start makes the review goal-aware. `ignore me`
when an attempt was a dead end. `list me` when you just want to peek at the
buffer without committing to a verb.

## User vs. agent attribution

A raw inotify watcher sees every write and cannot tell *your* edits apart
from Claude's own `Edit`/`Write`/`MultiEdit`/`NotebookEdit` tool calls.
Reviewing the agent's edits back to it is noise, so we filter them out:

1. A **`PostToolUse` hook** (`hooks/record_agent_edit.sh`) fires after every
   agent edit and appends `{ts_ns, path}` (absolute path) to the sidecar
   `.claude/work-with-me/agent-edits.jsonl`.
2. The watcher tails that sidecar. When an fs event arrives for a path with a
   recent agent record (within `AGENT_WINDOW_S`, default 4 s), it's
   **suppressed** as agent-authored. One agent record suppresses one event,
   so a genuine user edit of the same file later still lands in the log.
3. Anything not bracketed by an agent record → attributed to **you**.

If the hook isn't installed, the sidecar never appears and *every* change is
attributed to the user (the original, un-disambiguated behavior).

## Setup (no install.sh, no extra venv)

Deps go in the workspace venv:

```sh
.venv/bin/pip install -r contrib/skills/work-with-me/requirements.txt
```

Register the MCP server — merge `mcp.json.example` into the repo-root
`.mcp.json` (project scope, shared) or into `.claude/settings.local.json`
under an `mcpServers` key (local scope):

```json
"mcpServers": {
  "work-with-me": {
    "type": "stdio",
    "command": ".venv/bin/python",
    "args": ["contrib/skills/work-with-me/server.py"],
    "env": { "CLAUDE_PROJECT_DIR": "${CLAUDE_PROJECT_DIR}" }
  }
}
```

Register the attribution hook — add to `.claude/settings.json` (project) or
`.claude/settings.local.json` (local):

```json
"hooks": {
  "PostToolUse": [ {
    "matcher": "Edit|Write|MultiEdit|NotebookEdit",
    "hooks": [ { "type": "command",
      "command": "${CLAUDE_PROJECT_DIR}/contrib/skills/work-with-me/hooks/record_agent_edit.sh" } ]
  } ]
}
```

The hook needs `jq` on PATH. Restart Claude Code so it picks up the server +
hook. Verify with `claude mcp list` (should show `work-with-me`).

## Notes

- The change log lives in memory — resets when Claude Code restarts.
- The sidecar (`.claude/work-with-me/agent-edits.jsonl`) is grown by the hook
  and tailed by the watcher; both live under `.claude/`, which the watcher
  ignores. It's gitignored.
- If noisy files slip through, add them to `.gitignore`; the watcher reloads
  the rules immediately.
- The server process exits cleanly when Claude Code shuts it down.

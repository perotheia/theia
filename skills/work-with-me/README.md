# work-with-me

A Claude Code MCP plugin that watches **your** (the human's) file changes
and lets you ask Claude to review them for consistency at any point during a
session.

The plugin source is tracked under `skills/work-with-me/`; it is linked into
`.claude/plugins/work-with-me` (a local, gitignored symlink) so Claude Code
discovers it. It runs on the **workspace `.venv`** — no per-plugin venv.

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

| Tool | Trigger phrase | What it does |
|---|---|---|
| `check_me` | *"check me"* | Reviews your changes vs. session context, gives advice, **clears log** |
| `get_changes` | *"get changes"* | Shows the user-change log without clearing |
| `clear_changes` | *"clear changes"* | Manually resets the log |

`check_me` is the main loop: work → say "check me" → get advice → repeat.

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
.venv/bin/pip install -r skills/work-with-me/requirements.txt
```

Link the plugin into `.claude/` (local, gitignored):

```sh
mkdir -p .claude/plugins
ln -sfn ../../skills/work-with-me .claude/plugins/work-with-me
```

Register the MCP server — merge `mcp.json.example` into the repo-root
`.mcp.json` (project scope, shared) or into `.claude/settings.local.json`
under an `mcpServers` key (local scope):

```json
"mcpServers": {
  "work-with-me": {
    "type": "stdio",
    "command": ".venv/bin/python",
    "args": [".claude/plugins/work-with-me/server.py"],
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
      "command": "${CLAUDE_PROJECT_DIR}/.claude/plugins/work-with-me/hooks/record_agent_edit.sh" } ]
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

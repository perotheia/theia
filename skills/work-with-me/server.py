#!/usr/bin/env python3
"""
work-with-me — Claude Code MCP plugin
======================================
Watches file changes *you* (the user) make in the repo (honoring
.gitignore), keeps a running log, and exposes a 'check_me' tool that
reviews your modifications for consistency with the current session.

User vs. agent attribution
--------------------------
A raw inotify watcher cannot tell whether a write came from you (editing
in your editor) or from Claude's own Edit/Write tools — and reviewing the
agent's own edits back to it is noise. We disambiguate with a
``PostToolUse`` hook (see hooks/record_agent_edit.sh): every time the
agent edits a file, the hook appends ``{ts_ns, path}`` to a sidecar
JSONL. The watcher consults that sidecar and **suppresses** any fs event
whose path matches a recent agent record (within AGENT_WINDOW_S). Anything
not bracketed by an agent edit is attributed to the user and logged.

Tools
-----
check_me      Review + clear the user-change log. Trigger: "check me".
get_changes   Peek at the log without clearing it.
clear_changes Manually reset the log.
"""

import asyncio
import json
import os
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import pathspec
from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import Tool, TextContent
from watchdog.events import FileSystemEvent, FileSystemEventHandler
from watchdog.observers import Observer

# How long after an agent edit we treat an fs event on the same path as
# agent-authored (covers the watchdog → hook ordering jitter + editor
# fsync lag). Generous enough to swallow the agent's write, short enough
# that a real user edit seconds later still lands in the log.
AGENT_WINDOW_S = 4.0


# ──────────────────────────────────────────────────────────────────────────────
# Repo helpers
# ──────────────────────────────────────────────────────────────────────────────

def find_repo_root(start: Path) -> Path:
    """Resolve the repo root. Prefer $CLAUDE_PROJECT_DIR (set by Claude Code
    for both hooks and MCP servers); fall back to walking up for .git."""
    env_root = os.environ.get("CLAUDE_PROJECT_DIR")
    if env_root and Path(env_root).is_dir():
        return Path(env_root).resolve()
    for candidate in [start.resolve(), *start.resolve().parents]:
        if (candidate / ".git").exists():
            return candidate
    return start.resolve()


def load_gitignore_spec(root: Path) -> pathspec.PathSpec:
    """Build a PathSpec from root/.gitignore (always ignores .git/ and the
    plugin's own sidecar state under .claude/)."""
    patterns = [".git/", ".git", ".claude/"]
    gitignore = root / ".gitignore"
    if gitignore.exists():
        try:
            patterns += gitignore.read_text(encoding="utf-8").splitlines()
        except OSError:
            pass
    return pathspec.PathSpec.from_lines("gitwildmatch", patterns)


# ──────────────────────────────────────────────────────────────────────────────
# Agent-edit sidecar — the user/agent attribution channel
# ──────────────────────────────────────────────────────────────────────────────

class AgentEdits:
    """Reads the PostToolUse hook's append-only JSONL of agent edits and
    answers 'did the agent just touch this path?'. Tolerant of a missing
    file (no hook installed → everything attributed to the user)."""

    def __init__(self, sidecar: Path):
        self._sidecar = sidecar
        self._lock = threading.Lock()
        # path -> most-recent agent-edit monotonic deadline
        self._recent: dict[str, float] = {}
        self._pos = 0  # byte offset already consumed

    def _ingest(self) -> None:
        """Pull any new lines from the sidecar into _recent. Cheap to call
        on every fs event (only reads bytes appended since last time)."""
        try:
            size = self._sidecar.stat().st_size
        except OSError:
            return
        if size < self._pos:           # truncated/rotated → restart
            self._pos = 0
        if size == self._pos:
            return
        try:
            with self._sidecar.open("r", encoding="utf-8") as fh:
                fh.seek(self._pos)
                chunk = fh.read()
                self._pos = fh.tell()
        except OSError:
            return
        now = time.monotonic()
        for line in chunk.splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
                path = rec["path"]
            except (json.JSONDecodeError, KeyError, TypeError):
                continue
            # We can't trust the hook's wall-clock vs our monotonic clock,
            # so stamp the window from when WE observe the record. The hook
            # fires right after the agent's write, so this is tight enough.
            self._recent[path] = now + AGENT_WINDOW_S

    def is_agent(self, abs_path: str) -> bool:
        """True if `abs_path` was edited by the agent within the window.

        Time-windowed, not one-shot: a single agent edit usually fires
        several fs events (create+modify, or N modifies per save), so the
        record must suppress ALL of them until the window expires. A genuine
        user edit AFTER the window (default 4 s) still logs, because the
        record has lapsed by then; a fresh agent edit re-arms the window."""
        with self._lock:
            self._ingest()
            now = time.monotonic()
            self._recent = {p: d for p, d in self._recent.items() if d > now}  # prune
            return abs_path in self._recent


# ──────────────────────────────────────────────────────────────────────────────
# Thread-safe change log
# ──────────────────────────────────────────────────────────────────────────────

class ChangeLog:
    def __init__(self):
        self._lock = threading.Lock()
        self._entries: list[dict] = []

    def add(self, event_type: str, path: str, dest: str | None = None) -> None:
        entry: dict = {
            "time": datetime.now(timezone.utc).strftime("%H:%M:%S"),
            "type": event_type,
            "path": path,
        }
        if dest:
            entry["dest"] = dest
        with self._lock:
            # Deduplicate burst events: skip if identical to the last entry
            if self._entries:
                last = self._entries[-1]
                if (last["type"] == entry["type"]
                        and last["path"] == entry["path"]
                        and last.get("dest") == entry.get("dest")):
                    return
            self._entries.append(entry)

    def snapshot_and_clear(self) -> list[dict]:
        with self._lock:
            snap = list(self._entries)
            self._entries.clear()
            return snap

    def snapshot(self) -> list[dict]:
        with self._lock:
            return list(self._entries)

    def clear(self) -> int:
        with self._lock:
            n = len(self._entries)
            self._entries.clear()
            return n


# ──────────────────────────────────────────────────────────────────────────────
# Watchdog event handler
# ──────────────────────────────────────────────────────────────────────────────

class RepoHandler(FileSystemEventHandler):
    """Forwards user-authored watchdog events to ChangeLog, skipping
    .gitignore'd paths and agent-authored writes."""

    def __init__(self, root: Path, log: ChangeLog, agent: AgentEdits):
        super().__init__()
        self.root = root
        self.log = log
        self.agent = agent
        self._spec = load_gitignore_spec(root)
        self._spec_lock = threading.Lock()

    def reload_gitignore(self) -> None:
        with self._spec_lock:
            self._spec = load_gitignore_spec(self.root)

    def _ignored(self, abs_path: str) -> bool:
        try:
            rel = Path(abs_path).relative_to(self.root)
        except ValueError:
            return True
        with self._spec_lock:
            return self._spec.match_file(str(rel))

    def _rel(self, abs_path: str) -> str:
        try:
            return str(Path(abs_path).relative_to(self.root))
        except ValueError:
            return abs_path

    def _record(self, event_type: str, abs_path: str) -> None:
        """Log an event unless it's ignored or agent-authored."""
        if self._ignored(abs_path):
            return
        if self.agent.is_agent(abs_path):
            return  # Claude's own Edit/Write — not the user's; skip.
        self.log.add(event_type, self._rel(abs_path))

    # -- watchdog callbacks --------------------------------------------------

    def on_created(self, event: FileSystemEvent):
        if not event.is_directory:
            self._record("created", str(event.src_path))

    def on_modified(self, event: FileSystemEvent):
        if event.is_directory:
            return
        p = str(event.src_path)
        if Path(p).name == ".gitignore":
            self.reload_gitignore()
        self._record("modified", p)

    def on_deleted(self, event: FileSystemEvent):
        if not event.is_directory:
            self._record("deleted", str(event.src_path))

    def on_moved(self, event):
        if event.is_directory:
            return
        src, dst = str(event.src_path), str(event.dest_path)
        src_ign, dst_ign = self._ignored(src), self._ignored(dst)
        if src_ign and dst_ign:
            return
        # Treat an agent-authored move (rename via tool) the same as an edit.
        if self.agent.is_agent(dst) or self.agent.is_agent(src):
            return
        self.log.add(
            "moved",
            self._rel(src) if not src_ign else src,
            self._rel(dst) if not dst_ign else dst,
        )


# ──────────────────────────────────────────────────────────────────────────────
# Formatting helpers
# ──────────────────────────────────────────────────────────────────────────────

def _render_log(entries: list[dict]) -> str:
    if not entries:
        return "(none)"
    lines = []
    for e in entries:
        if "dest" in e:
            lines.append(f"  [{e['time']}] {e['type'].upper():<8}  {e['path']}  →  {e['dest']}")
        else:
            lines.append(f"  [{e['time']}] {e['type'].upper():<8}  {e['path']}")
    return "\n".join(lines)


def _group_by_path(entries: list[dict]) -> dict[str, list[str]]:
    """Return {path: [event_types]} for the check_me prompt."""
    grouped: dict[str, list[str]] = {}
    for e in entries:
        key = e["path"]
        grouped.setdefault(key, []).append(e["type"])
        if "dest" in e:
            grouped.setdefault(e["dest"], []).append("moved-here")
    return grouped


# ──────────────────────────────────────────────────────────────────────────────
# MCP server
# ──────────────────────────────────────────────────────────────────────────────

app = Server("work-with-me")
changelog = ChangeLog()
_observer: "Observer | None" = None


@app.list_tools()
async def list_tools() -> list[Tool]:
    return [
        Tool(
            name="check_me",
            description=(
                "Review all file modifications the USER has made since the last checkpoint "
                "for consistency with the current session context. (Edits made by the agent's "
                "own Edit/Write tools are excluded — this reviews the human's manual changes.) "
                "Identify issues such as: incomplete edits, missing counterpart changes "
                "(updated a call site but not the definition, or vice versa), broken imports, "
                "type / signature mismatches, or logic gaps. "
                "Provide actionable advice on what to fix. "
                "Clears the change log after reviewing (starts a fresh checkpoint). "
                "Call this tool when the user says 'check me'."
            ),
            inputSchema={"type": "object", "properties": {}, "required": []},
        ),
        Tool(
            name="get_changes",
            description=(
                "Return the accumulated USER file-change log since the last checkpoint "
                "without clearing it. Useful for a quick peek at what has changed."
            ),
            inputSchema={"type": "object", "properties": {}, "required": []},
        ),
        Tool(
            name="clear_changes",
            description="Manually reset the file change log and start a fresh checkpoint.",
            inputSchema={"type": "object", "properties": {}, "required": []},
        ),
    ]


@app.call_tool()
async def call_tool(name: str, arguments: dict[str, Any]) -> list[TextContent]:

    # ── check_me ────────────────────────────────────────────────────────────
    if name == "check_me":
        entries = changelog.snapshot_and_clear()
        if not entries:
            return [TextContent(
                type="text",
                text=(
                    "No USER file changes recorded since the last checkpoint.\n"
                    "(Agent edits are tracked separately and excluded.)\n"
                    "The change log is now cleared — fresh checkpoint started."
                ),
            )]

        log_text = _render_log(entries)
        grouped = _group_by_path(entries)
        files_touched = "\n".join(
            f"  {path}  ({', '.join(ops)})" for path, ops in grouped.items()
        )

        prompt = (
            "## work-with-me checkpoint\n\n"
            f"**Files the USER touched since last checkpoint:**\n{files_touched}\n\n"
            f"**Full event log:**\n{log_text}\n\n"
            "---\n"
            "Please review the user's changes above in the context of this session.\n\n"
            "Check for:\n"
            "- Incomplete edits (a rename/refactor applied in one place but not others)\n"
            "- Missing counterpart changes (call site updated but not the function, or vice versa)\n"
            "- Import / export mismatches introduced by file additions or deletions\n"
            "- Type / signature drift\n"
            "- Deleted files that are still referenced\n"
            "- Any logic gaps visible from the file list\n\n"
            "Give concrete, actionable advice for each issue found. "
            "If everything looks consistent, say so.\n\n"
            "_Change log cleared — next checkpoint starts now._"
        )
        return [TextContent(type="text", text=prompt)]

    # ── get_changes ─────────────────────────────────────────────────────────
    elif name == "get_changes":
        entries = changelog.snapshot()
        if not entries:
            return [TextContent(type="text", text="No user changes recorded yet.")]
        return [TextContent(
            type="text",
            text=f"## Current user-change log ({len(entries)} events)\n\n{_render_log(entries)}",
        )]

    # ── clear_changes ────────────────────────────────────────────────────────
    elif name == "clear_changes":
        n = changelog.clear()
        return [TextContent(
            type="text",
            text=f"Change log cleared ({n} events removed). Fresh checkpoint started.",
        )]

    return [TextContent(type="text", text=f"Unknown tool: {name}")]


# ──────────────────────────────────────────────────────────────────────────────
# Entry point
# ──────────────────────────────────────────────────────────────────────────────

async def main() -> None:
    global _observer

    root = find_repo_root(Path.cwd())
    sidecar = root / ".claude" / "work-with-me" / "agent-edits.jsonl"
    sidecar.parent.mkdir(parents=True, exist_ok=True)
    agent = AgentEdits(sidecar)
    handler = RepoHandler(root, changelog, agent)

    _observer = Observer()
    _observer.schedule(handler, str(root), recursive=True)
    _observer.start()

    try:
        async with stdio_server() as (read_stream, write_stream):
            await app.run(
                read_stream,
                write_stream,
                app.create_initialization_options(),
            )
    finally:
        _observer.stop()
        _observer.join()


if __name__ == "__main__":
    asyncio.run(main())

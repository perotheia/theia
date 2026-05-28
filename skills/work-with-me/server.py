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
import bisect
import json
import os
import subprocess
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

# Cap how many shell commands a checkpoint reports, so a huge history burst
# can't blow up the check_me payload.
SHELL_MAX = 200

# Correlate shell commands to THIS repo's file activity by time, so a shared
# history full of commands from other terminals doesn't accumulate. A command
# at time Tc is kept if some file-change event at Tf falls in
# [Tc - SHELL_PRE_S, Tc + SHELL_POST_S]:
#   PRE  — you ran something, then edited a file (small lookback).
#   POST — a command that ITSELF writes files (generator/compiler/cp/git):
#          its outputs land seconds AFTER the command, so the file events sit
#          in the command's near future. Wider than PRE.
SHELL_PRE_S = 3.0
SHELL_POST_S = 20.0


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
# Shell history tail — what the user is running in their terminals
# ──────────────────────────────────────────────────────────────────────────────

class ShellHistory:
    """Tails the shared zsh history file ($HISTFILE, default ~/.zsh_history),
    returning the commands appended since the last checkpoint. zsh's history
    is append-only and common to all terminals, so this is a single
    chronological feed of what the USER typed at the shell — context that
    pairs with the file-change log (e.g. 'edited X' alongside 'ran build Y').

    Read-only: never writes the history file. Tolerant of a missing file
    (no shell history → empty feed). Parses zsh's EXTENDED_HISTORY format
    `: <epoch>:<elapsed>;<command>` and falls back to plain lines."""

    def __init__(self, histfile: Path):
        self._histfile = histfile
        self._lock = threading.Lock()
        # Start the checkpoint at the current end of file so we only ever
        # report commands run AFTER the watcher started.
        try:
            self._pos = histfile.stat().st_size
        except OSError:
            self._pos = 0

    @staticmethod
    def _parse(line: str) -> "tuple[int | None, str] | None":
        """(epoch|None, command) for one history line, or None to skip."""
        line = line.rstrip("\n")
        if not line.strip():
            return None
        if line.startswith(":"):
            # `: 1700000000:0;the command`
            try:
                meta, cmd = line.split(";", 1)
                epoch = int(meta.split(":")[1].strip())
                return epoch, cmd
            except (ValueError, IndexError):
                return None, line
        return None, line

    def _read_new(self) -> list[dict]:
        """Pull entries appended since last read; advance the offset.
        Each entry is {"ts": epoch|None, "cmd": str}."""
        try:
            size = self._histfile.stat().st_size
        except OSError:
            return []
        if size < self._pos:       # history rotated/truncated → restart
            self._pos = 0
        if size == self._pos:
            return []
        try:
            # zsh writes latin-1-ish bytes for meta-quoted multibyte chars;
            # decode leniently so a stray byte never drops a command.
            with self._histfile.open("r", encoding="utf-8", errors="replace") as fh:
                fh.seek(self._pos)
                chunk = fh.read()
                self._pos = fh.tell()
        except OSError:
            return []
        out: list[dict] = []
        # zsh joins multi-line commands with a trailing backslash; keep it
        # simple — one history entry per physical line is the common case.
        for raw in chunk.splitlines():
            parsed = self._parse(raw)
            if parsed is None:
                continue
            epoch, cmd = parsed
            cmd = cmd.strip()
            if cmd:
                out.append({"ts": epoch, "cmd": cmd})
        return out

    def snapshot_and_advance(self) -> list[dict]:
        """Entries since the last checkpoint; advance past them (clears)."""
        with self._lock:
            return self._read_new()

    def peek(self) -> list[dict]:
        """Entries since the last checkpoint WITHOUT advancing (get_changes).
        We read new lines, then rewind the offset so they're reported again
        on the next call until a checkpoint consumes them."""
        with self._lock:
            before = self._pos
            entries = self._read_new()
            self._pos = before
            return entries


# ──────────────────────────────────────────────────────────────────────────────
# Thread-safe change log
# ──────────────────────────────────────────────────────────────────────────────

class ChangeLog:
    def __init__(self):
        self._lock = threading.Lock()
        self._entries: list[dict] = []

    def add(self, event_type: str, path: str, dest: str | None = None) -> None:
        now = datetime.now(timezone.utc)
        entry: dict = {
            "ts": now.timestamp(),               # epoch — for shell-window correlation
            "time": now.strftime("%H:%M:%S"),    # display
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
        """Log an event unless tracking is off, it's ignored, or agent-authored."""
        if not state.is_enabled():
            return  # 'watch me off' (default) — non-intrusive.
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


def _window_filter(cmds: list[dict], file_ts: list[float]) -> list[dict]:
    """Keep only commands time-correlated to this repo's file activity, so a
    shared history full of other-terminal commands doesn't accumulate.

    A command at Tc is kept if some file event Tf satisfies
        Tc - SHELL_PRE_S  ≤  Tf  ≤  Tc + SHELL_POST_S
    (asymmetric: a file-writing command's outputs land AFTER it → POST wide).

    If there are no file events to anchor against, OR a command carries no
    timestamp (plain history line), we fail OPEN and keep it — better a little
    noise than dropping a relevant command."""
    if not file_ts:
        return cmds
    ft = sorted(file_ts)
    kept: list[dict] = []
    for c in cmds:
        tc = c.get("ts")
        if tc is None:
            kept.append(c)            # untimestamped → can't window, keep
            continue
        lo, hi = tc - SHELL_PRE_S, tc + SHELL_POST_S
        # any file event within [lo, hi]?  (bisect over sorted ft)
        i = bisect.bisect_left(ft, lo)
        if i < len(ft) and ft[i] <= hi:
            kept.append(c)
    return kept


def _render_shell(cmds: list[dict]) -> str:
    """Numbered list of shell commands, capped at SHELL_MAX (keep the most
    recent — those are closest to the edits being reviewed). Each entry is
    {"ts": epoch|None, "cmd": str}."""
    if not cmds:
        return "(none)"
    shown = cmds[-SHELL_MAX:]
    elided = len(cmds) - len(shown)
    lines = []
    if elided:
        lines.append(f"  … {elided} earlier command(s) elided …")
    base = elided + 1
    for i, c in enumerate(shown, start=base):
        ts = c.get("ts")
        clock = datetime.fromtimestamp(ts, timezone.utc).strftime("%H:%M:%S") \
            if ts else "--:--:--"
        lines.append(f"  {i:>4}. [{clock}] {c['cmd']}")
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
shellhist: "ShellHistory | None" = None   # set in main() once HISTFILE is known
_observer: "Observer | None" = None


class State:
    """Tracking on/off + the optional focus note.

    enabled=False is the project-friendly default: the watcher runs (so the
    moment the user says 'watch me on' nothing has to spin up) but events are
    dropped at _record, and the shell tail's offset is advanced rather than
    accumulated. Non-intrusive for teammates who share .mcp.json."""

    def __init__(self):
        self._lock = threading.Lock()
        self.enabled: bool = False
        self.focus: str = ""
        self.root: Path = Path.cwd()

    def set_enabled(self, on: bool) -> None:
        with self._lock:
            self.enabled = on

    def is_enabled(self) -> bool:
        with self._lock:
            return self.enabled

    def set_focus(self, note: str) -> None:
        with self._lock:
            self.focus = note.strip()

    def get_focus(self) -> str:
        with self._lock:
            return self.focus


state = State()


def _histfile() -> Path:
    """The shared zsh history file: $HISTFILE, else ~/.zsh_history."""
    env = os.environ.get("HISTFILE")
    return Path(env).expanduser() if env else Path.home() / ".zsh_history"


def _git_diff(paths: list[str], *, cwd: Path) -> str:
    """`git diff -- <paths>` against working tree (vs HEAD). Returns the diff
    text or a short note. Bounded to a reasonable size so the prompt stays
    manageable; if any path isn't tracked, `git diff` simply omits it."""
    if not paths:
        return "(no paths)"
    try:
        out = subprocess.run(
            ["git", "diff", "--no-color", "--", *paths],
            cwd=str(cwd), capture_output=True, text=True, timeout=10,
        ).stdout
    except (OSError, subprocess.TimeoutExpired) as e:
        return f"(git diff failed: {e})"
    if not out.strip():
        return "(no diff vs HEAD — either staged, new untracked, or unchanged)"
    MAX = 60_000
    if len(out) > MAX:
        out = out[:MAX] + f"\n… (truncated at {MAX} chars)\n"
    return out


@app.list_tools()
async def list_tools() -> list[Tool]:
    return [
        Tool(
            name="watch_me",
            description=(
                "Turn the watcher ON: begin recording the user's file edits + shell "
                "commands for the current session. Starts a fresh checkpoint. Plugin is "
                "OFF by default so it's non-intrusive for teammates who share .mcp.json — "
                "say 'watch me' to start a session. If already on, reports current "
                "buffered counts. Trigger: 'watch me'. (To stop + discard everything, "
                "use 'ignore me'.)"
            ),
            inputSchema={"type": "object", "properties": {}, "required": []},
        ),
        Tool(
            name="check_me",
            description=(
                "Review the USER's file modifications since the last checkpoint for "
                "consistency with the current session, ALONGSIDE the shell commands they "
                "ran (tailed from the shared zsh history, time-correlated to the file "
                "edits — commands from other terminals doing unrelated work are dropped). "
                "Agent Edit/Write/MultiEdit/NotebookEdit calls are excluded. If a FOCUS "
                "is set, it appears as the explicit goal of the session. Identify: "
                "incomplete edits, missing counterpart changes, broken imports, type / "
                "signature drift, logic gaps, or edits inconsistent with what the "
                "commands or focus imply. Provide actionable advice. Clears the change "
                "log + shell tail (starts a fresh checkpoint). Trigger: 'check me'."
            ),
            inputSchema={"type": "object", "properties": {}, "required": []},
        ),
        Tool(
            name="ignore_me",
            description=(
                "Turn the watcher OFF and DISCARD everything buffered since the last "
                "'watch me' — file changes and shell commands — WITHOUT a review. Use "
                "this when the work was exploratory / an aborted attempt / not what you "
                "want considered. Plugin stays quiet until you say 'watch me' again. "
                "Trigger: 'ignore me'."
            ),
            inputSchema={"type": "object", "properties": {}, "required": []},
        ),
        Tool(
            name="focus_me",
            description=(
                "Set the one-line goal for the current session. The next check_me / "
                "compare_me embeds it as the explicit intent (e.g. 'am I still on track "
                "for X?'). Pass note='' (empty) to clear the focus. Trigger: 'focus me on "
                "<note>' / 'set focus to <note>' / 'clear focus'."
            ),
            inputSchema={
                "type": "object",
                "properties": {"note": {"type": "string"}},
                "required": [],
            },
        ),
        Tool(
            name="compare_me",
            description=(
                "Like check_me, but instead of just listing the touched paths, include "
                "the actual `git diff` of those files (working tree vs HEAD) so the "
                "review sees the change CONTENT — catches typos, broken refs, signature "
                "drift the file list can't. Clears the checkpoint. Trigger: 'compare me' "
                "/ 'diff me'."
            ),
            inputSchema={"type": "object", "properties": {}, "required": []},
        ),
        Tool(
            name="undo_me",
            description=(
                "Report the unstaged diff for the files the USER touched since the last "
                "checkpoint, with a `git checkout -- <path>` revert hint per file. A "
                "non-destructive 'I made a mess, what did I change?' lifeline — does NOT "
                "run any revert itself. Does NOT clear the checkpoint. Trigger: 'undo me'."
            ),
            inputSchema={"type": "object", "properties": {}, "required": []},
        ),
    ]


def _build_checkpoint(*, with_diff: bool) -> tuple[list[dict], list[dict], str]:
    """Drain the file-change log + correlated shell tail. Returns
    (entries, cmds, files_block) ready for prompt assembly. Used by both
    check_me (with_diff=False) and compare_me (with_diff=True)."""
    entries = changelog.snapshot_and_clear()
    cmds = shellhist.snapshot_and_advance() if shellhist else []
    cmds = _window_filter(cmds, [e["ts"] for e in entries if "ts" in e])

    files_block = "(no file changes)"
    if entries:
        grouped = _group_by_path(entries)
        files_touched = "\n".join(
            f"  {path}  ({', '.join(ops)})" for path, ops in grouped.items()
        )
        files_block = f"{files_touched}\n\n**Full event log:**\n{_render_log(entries)}"
        if with_diff:
            paths = sorted({e["path"] for e in entries} |
                           {e["dest"] for e in entries if "dest" in e})
            diff = _git_diff(paths, cwd=state.root)
            files_block += f"\n\n**git diff (working tree vs HEAD):**\n```diff\n{diff}\n```"
    return entries, cmds, files_block


@app.call_tool()
async def call_tool(name: str, arguments: dict[str, Any]) -> list[TextContent]:
    args = arguments or {}

    # ── watch_me ── start tracking (or report status if already on) ─────────
    if name == "watch_me":
        if state.is_enabled():
            # Already tracking: useful to report current buffer + focus rather
            # than be a silent no-op. Doesn't drain or restart.
            ne = len(changelog.snapshot())
            nc = len(shellhist.peek()) if shellhist else 0
            focus = state.get_focus()
            focus_line = f"\nFocus: {focus}" if focus else "\nFocus: (none)"
            return [TextContent(type="text", text=(
                f"Already watching. Buffered: {ne} file event(s), "
                f"{nc} shell command(s){focus_line}\n"
                f"(`check me` to review, `ignore me` to discard.)"
            ))]
        state.set_enabled(True)
        return [TextContent(type="text", text=(
            "Watching ON — file edits + shell commands will be tracked from now. "
            "Fresh checkpoint started. (`check me` to review, `ignore me` to discard.)"
        ))]

    # ── check_me ────────────────────────────────────────────────────────────
    if name == "check_me":
        if not state.is_enabled():
            return [TextContent(type="text", text=(
                "Watching is OFF — nothing has been tracked.\n"
                "Say 'watch me on' to start a session, then 'check me' to review."
            ))]
        entries, cmds, files_block = _build_checkpoint(with_diff=False)
        if not entries and not cmds:
            return [TextContent(type="text", text=(
                "No USER file changes or correlated shell commands since the last "
                "checkpoint.\n(Agent edits are tracked separately and excluded.)\n"
                "The change log is now cleared — fresh checkpoint started."
            ))]

        focus = state.get_focus()
        focus_block = f"\n**Session focus (explicit goal):** {focus}\n" if focus else ""

        prompt = (
            "## work-with-me checkpoint\n"
            f"{focus_block}\n"
            f"**Files the USER touched since last checkpoint:**\n{files_block}\n\n"
            f"**Shell commands time-correlated to those edits (zsh history):**\n"
            f"{_render_shell(cmds)}\n\n"
            "---\n"
            "Please review the user's changes above in the context of this session.\n"
            "Use the shell commands as intent signal — what they were building, "
            "running, or debugging — and cross-check it against the file edits"
            + (" and the stated focus" if focus else "")
            + ".\n\nCheck for:\n"
            "- Incomplete edits (a rename/refactor applied in one place but not others)\n"
            "- Missing counterpart changes (call site updated but not the function, or vice versa)\n"
            "- Import / export mismatches introduced by file additions or deletions\n"
            "- Type / signature drift\n"
            "- Deleted files that are still referenced\n"
            "- Edits inconsistent with what the shell commands"
            + (" or the focus" if focus else "")
            + " suggest they intended\n"
            "- A command that failed or implies a step not yet done\n\n"
            "Give concrete, actionable advice for each issue found. "
            "If everything looks consistent, say so.\n\n"
            "_Change log + shell tail cleared — next checkpoint starts now._"
        )
        return [TextContent(type="text", text=prompt)]

    # ── ignore_me ── stop tracking + discard everything buffered ───────────
    if name == "ignore_me":
        was_on = state.is_enabled()
        state.set_enabled(False)
        ne = changelog.clear()
        cmds = shellhist.snapshot_and_advance() if shellhist else []
        prefix = "Watching OFF" if was_on else "Already off"
        return [TextContent(type="text", text=(
            f"{prefix}. Discarded {ne} file event(s) and {len(cmds)} shell "
            f"command(s) without review. (`watch me` to start again.)"
        ))]

    # ── focus_me ────────────────────────────────────────────────────────────
    if name == "focus_me":
        note = (args.get("note") or "").strip()
        state.set_focus(note)
        if not note:
            return [TextContent(type="text", text="Focus cleared.")]
        return [TextContent(type="text", text=(
            f"Focus set: {note}\n(check_me / compare_me will include this as the "
            f"explicit goal until cleared.)"
        ))]

    # ── compare_me ──────────────────────────────────────────────────────────
    if name == "compare_me":
        if not state.is_enabled():
            return [TextContent(type="text", text=(
                "Watching is OFF — nothing has been tracked. Say 'watch me on' first."
            ))]
        entries, cmds, files_block = _build_checkpoint(with_diff=True)
        if not entries and not cmds:
            return [TextContent(type="text", text=(
                "No USER changes to compare. Checkpoint cleared."
            ))]

        focus = state.get_focus()
        focus_block = f"\n**Session focus (explicit goal):** {focus}\n" if focus else ""

        prompt = (
            "## work-with-me compare\n"
            f"{focus_block}\n"
            f"**Files + diff:**\n{files_block}\n\n"
            f"**Correlated shell commands:**\n{_render_shell(cmds)}\n\n"
            "---\n"
            "Review the diff above against the user's intent (the shell commands"
            + (" and the stated focus" if focus else "")
            + "). Comment line-by-line where it matters: bugs, typos, broken refs, "
            "signature drift, incomplete edits, contradictions with the goal.\n\n"
            "_Checkpoint cleared._"
        )
        return [TextContent(type="text", text=prompt)]

    # ── undo_me ─────────────────────────────────────────────────────────────
    if name == "undo_me":
        # Peek (don't clear): undo_me is a lifeline, not a checkpoint event.
        entries = changelog.snapshot()
        if not entries:
            return [TextContent(type="text", text=(
                "No tracked user changes to undo. Use `git status` to see anything "
                "outside the current checkpoint."
            ))]
        paths = sorted({e["path"] for e in entries} |
                       {e["dest"] for e in entries if "dest" in e})
        diff = _git_diff(paths, cwd=state.root)
        revert = "\n".join(f"  git checkout -- {p}" for p in paths)
        text = (
            "## undo_me — what you changed (and how to revert)\n\n"
            f"**Touched files ({len(paths)}):**\n"
            + "\n".join(f"  {p}" for p in paths) + "\n\n"
            f"**Diff vs HEAD:**\n```diff\n{diff}\n```\n\n"
            "**To revert any of them (does NOT run; copy what you want):**\n"
            f"```sh\n{revert}\n```\n\n"
            "_Checkpoint NOT cleared._"
        )
        return [TextContent(type="text", text=text)]

    return [TextContent(type="text", text=f"Unknown tool: {name}")]


# ──────────────────────────────────────────────────────────────────────────────
# Entry point
# ──────────────────────────────────────────────────────────────────────────────

async def main() -> None:
    global _observer, shellhist

    root = find_repo_root(Path.cwd())
    state.root = root
    sidecar = root / ".claude" / "work-with-me" / "agent-edits.jsonl"
    sidecar.parent.mkdir(parents=True, exist_ok=True)
    agent = AgentEdits(sidecar)
    handler = RepoHandler(root, changelog, agent)

    # Shell-history tail: the checkpoint baseline is "now", so we only ever
    # report commands the user runs after the watcher starts.
    shellhist = ShellHistory(_histfile())

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

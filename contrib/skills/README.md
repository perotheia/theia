# Skills

Project skills for Claude Code, tracked here in `docs/skills/` so they're
versioned with the repo and shared across the team. Each subdirectory is one
skill (a `SKILL.md` plus optional `references/`).

| Skill | What it's for |
| --- | --- |
| [`theia/`](theia/SKILL.md) | Orientation refresher for the whole project — the `.art` DSL, FCs, the Theia runtime, the supervisor, and the .art→manifest→build→deploy pipeline. Start here. |
| [`tasks/`](tasks/) | The `docs/tasks/{BACKLOG,TODO,PROGRESS,DONE}` task-note workflow. |

## First-time setup: link them into `.claude/skills/`

Claude Code discovers project skills under `.claude/skills/`. That directory
is **local agent state** (gitignored), so a fresh checkout has to relink the
tracked skills into it once. From the repo root:

```sh
mkdir -p .claude/skills
# Prune any dangling links (e.g. a skill that was renamed or removed).
for l in .claude/skills/*; do [ -e "$l" ] || rm -f "$l"; done
# (Re)link every tracked skill.
for d in docs/skills/*/; do
    name=$(basename "$d")
    ln -sfn "../../docs/skills/$name" ".claude/skills/$name"
done
```

This is fully idempotent and self-healing: the first loop drops links whose
target is gone, the second adds/repoints one per tracked skill. `ln -sfn`
(`-f` replace, `-n` don't descend into a present target dir). The relative
target `../../docs/skills/<name>` resolves from `.claude/skills/` back to the
repo root — keep that depth if you add links by hand.

Verify nothing dangles:

```sh
for l in .claude/skills/*; do [ -e "$l" ] || echo "DANGLING: $l -> $(readlink "$l")"; done
```

Then reload skills in your session (`/skills`, or restart Claude Code) so the
new set is picked up.

## When the skill set changes

After you add, remove, or rename a skill under `docs/skills/`, re-run the
two-loop snippet above — it resyncs `.claude/skills/` in both directions
(prunes danglers, links new names).

# project layout housekeeping

Done items (2026-05-27, branch `housekeeping-layout`):

1. ~~task.md frontend for docs/tasks doesn't support nested task dirs —
   keep 1-level dir structure.~~ **DONE.** Flattened PROGRESS + DONE:
   lone `README.md` → `<dir>.md`, extra files → `<dir>-<leaf>.md`.

6. ~~supervisor-gui/ → tools/~~ **DONE** → `tools/supervisor-gui/`. (Surfaced a
   pre-existing build break — stale `services::com::TraceStream` — filed as
   `supervisor-gui-tracestream-repair.md`. The move itself is clean.)

7. ~~config/ → rules/config — if bazel agree.~~ **DONE.** Bazel agrees:
   `//config:{host,is_amd64,is_arm64,rpi4}` → `//rules/config:...`, all resolve;
   `--platforms=//rules/config:rpi4` analyzes. Updated `tools/deploy_rpi4.sh`.

---

## Remaining (the big //system move + FC spec/impl colocation — deferred)

These four are an interlocked .art/loader/symlink refactor; do them as a
focused pass.

2. system to root catalog
   //platform/system -> //system

3. services/system/system.art -> //system/services/cluster.art

4. distribute services/system/<fc> into implementation dirs as
   services/<fc>/system/<fc>/{package,component}.art

5. relink services/<fc>/system/<fc> -> //system/services/<fc>

Current state for reference:
- `platform/system/` is a real dir of symlinks (autosar, demo, gateway,
  odd_path_client, services, supervisor) + a BUILD.bazel — the aggregation
  point the artheia loader + `cluster Platform` in system.art resolve against.
- `services/system/<fc>/{package,component}.art` is the spec layer today
  (~18 FCs); `services/<fc>/` is the gen-app impl layer (lib/main/impl).
  Item 4 colocates spec under impl.

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

## The big //system move + FC spec/impl colocation — **DONE** (2026-05-27)

The four interlocked .art/loader/symlink moves, done as one focused pass:

2. ~~system to root catalog //platform/system -> //system~~ **DONE.**
   `system/{system.art,BUILD.bazel}` is the new virtual root; the stale
   `platform/system/` symlink dir is retired.

3. ~~services/system/system.art -> //system/services/cluster.art~~ **DONE.**
   The services aggregator is `system/services/cluster.art` (`package
   system.services`, `cluster Services`). The artheia import resolver gained
   `cluster.art` in its filename-priority list so `import system.services.*`
   finds it.

4. ~~distribute services/system/<fc> into services/<fc>/system/<fc>/
   {package,component}.art~~ **DONE.** The 6 daemon FCs (com, log, per, sm,
   ucm, shwa) colocate spec with impl. The 12 daemon-less placeholders moved
   together to `services/nop/` ("not operation"). `exec` is the AUTOSAR
   placeholder realized BY the supervisor: it keeps `node atomic ExecDaemon`
   (tipc 0x80010005) for the manifest binding, `import system.supervisor.*`
   in both files, a forward-decl'd `message SupervisionEvent` (package.art)
   and `node atomic Supervisor` (component.art), and a `composition Exec {
   prototype Supervisor exec }`.

5. ~~relink services/<fc>/system/<fc> -> //system/services/<fc>~~ **DONE.**
   `system/services/<fc>` symlinks aggregate all 18 FCs (6 impl + 12 nop);
   `system/{autosar,demo,gateway,odd_path_client,supervisor}` symlink to
   their owning trees.

artheia loader/LSP/Bazel repointed (separate artheia-repo commit):
`_default_art_root` → `system/services`; LSP marker → `system/services/`;
`//platform/system:art_sources` → `//system:art_sources`; merger now hoists
`import` lines from BOTH package.art + component.art so a placeholder FC can
import another package for a forward-decl.

Verified: `artheia parse system/system.art` (full tree), PlatformServices
load (18 FCs), executor emit, gen-netgraph, fc_regen_stability 5/5
byte-identical, artheia pytest 169/1, `bazel build //system:art_sources` +
`@rig_demo//:executor_json`, host build of all 5 FC daemons.

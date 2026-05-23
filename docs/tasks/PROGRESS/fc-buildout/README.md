# FC build-out — implement SM's cooperation partners

Goal: every cooperation partner SM names in
`docs/autosar/services/sm.md §3.B` becomes a real `cc_binary` —
EXEC, COM, UCM, PER. Each follows the SM pattern (GenServer /
GenStateM derivative + handle_call/cast + subscriber wiring) and
appears in the supervisor's `executor.yaml`.

This unlocks the next-tier conversation: cross-FC integration tests
where SM drives state changes and partners actually react.

## What changed in scope vs the sm phase

| Aspect | sm phase (done) | This phase |
|---|---|---|
| Messages | hand-rolled POD in `sm_messages.hh` | `artheia gen-proto` → `platform/proto/system/services/<fc>/*.pb.{h,c}` |
| start_cmd | hardcoded `services/sm/daemon.sh` fallback | dropped; explicit `Process.start_cmd` field |
| Binaries | 1 (sm) | +4 (exec, com, ucm, per) |
| Cross-FC messages | `from_sm` receiver decls only | full message types per cooperation arrow |

## Phases (commit boundaries)

### A — gen-proto wiring (artheia + theia)

1. Confirm `artheia gen-proto` emits what we need. **Done in survey:**
   ```
   $ artheia gen-proto services/system/sm/package.art --out /tmp/x
   /tmp/x/SmStateMsg.proto
   /tmp/x/SystemBoot.proto
   ...
   ```
   Output is flat under `--out`. Each .proto has
   `package services.services.sm;` (note the `system → services`
   rewrite for C++ namespace safety — see
   `artheia/artheia/generators/proto.py:_PROTO_PACKAGE_LEAD_RENAMES`).
   This is intentional, not a bug.

2. Decide layout: each FC owns
   `platform/proto/system/services/<fc>/` (mirror of
   `system.services.<fc>` .art package).

3. Bazel: each `platform/proto/system/services/<fc>/BUILD.bazel`
   exposes `<msg>_pb_c` and `<msg>_pb_h` filegroups (mirroring
   `platform/proto/gateway/system/BUILD.bazel`).
   `platform/proto/BUILD.bazel` rolls them into the existing
   `:platform_protos` cc_library.

4. Today: nanopb's `.pb.c` / `.pb.h` are committed alongside the
   `.proto` (one-shot via system `nanopb_generator`). A future
   genrule-driven regen lands later; for this phase we manually
   regen and commit, same as the gateway/system pattern.

### B — sm migration to gen-proto'd headers (theia)

1. Run `artheia gen-proto services/system/sm/package.art --out
   platform/proto/system/services/sm/`.
2. Run `nanopb_generator` on each .proto to emit .pb.{c,h}.
3. Write `platform/proto/system/services/sm/BUILD.bazel`.
4. Add to `platform/proto/BUILD.bazel`.
5. Delete `services/system/sm/include/sm/sm_messages.hh`.
6. Update `services/system/sm/include/sm/sm_daemon.hh` and
   `SmDaemonStateMBase.hh` to `#include "system/services/sm/SmStateMsg.pb.h"`.
   This means using nanopb's C struct shape — the underscored names
   like `services_services_sm_SmStateMsg` need a using-decl or alias.
7. Re-run `bazel test //services/system/sm:test_sm_daemon` — must
   pass unchanged.

### C — drop daemon.sh (artheia + theia)

1. artheia: add `Process.start_cmd: list[str]` field. When set, the
   manifest emission uses it; when empty, `_fc_child` emits an
   empty `start_cmd` and logs a build-time warning (`fc <name> has
   no start_cmd — supervisor will refuse to start it`).
2. Remove the `services/{short}/daemon.sh` default fallback.
3. Make `services.manifest.service` / `demo.manifest.rig` set
   `start_cmd=["bazel-bin/services/system/sm/sm"]` for sm (or via
   an Override mechanism — TBD when implementing).
4. Run `artheia executor emit --machine central_host
   demo.manifest.rig` and confirm sm's start_cmd is the real path.

### D — cross-FC message declarations (theia)

For each cooperation arrow in `sm.md §3.B`, declare the
corresponding message types in the *receiver's* .art:

| Arrow | Where to declare | Messages |
|---|---|---|
| SM → EM "Function Group state" | `services/system/exec/package.art` | `FunctionGroupRequest{group, state}` + `interface clientServer ExecCtl` |
| SM → COM "network gating" | `services/system/com/package.art` | `NetworkBindingRequest{enabled}` + `interface clientServer ComCtl` |
| SM → UCM "update mode" | `services/system/ucm/package.art` | UCM already has `UpdateCtl`; just add `UpdateModeRequest` |
| SM → PER "persist state" | `services/system/per/package.art` | `WriteRequest{key,value}`, `ReadRequest{key}`, `interface clientServer PersistencyIf` (replaces current empty forward decl) |
| EM → PHM "crash report" | `services/system/phm/package.art` | not in scope this phase |
| DM → SM "DTC" + NM → SM "wakeup" | `services/system/sm/package.art` | inbound to SM — not in scope this phase |

### E — binaries for EXEC, COM, UCM, PER (theia)

For each FC, following the sm pattern:

```
services/system/<fc>/
  include/<fc>/<Fc>Daemon.hh    # GenServer derivative
  src/main.cc                    # entry + stderr subscriber + signal handler
  test/test_<fc>_daemon.cc       # in-process integration test
  BUILD.bazel                    # cc_library + cc_binary + cc_test
```

Per-FC specifics:

| FC | Subscribes to SmStateStream | Server port | Action on state |
|---|---|---|---|
| **EXEC** | yes | `ExecCtl::StartGroup/StopGroup` | log received SmStateMsg; future: spawn supervised processes |
| **COM** | yes | `ComCtl::EnableBindings` | log; future: enable/disable network discovery |
| **UCM** | yes | `UpdateCtl::RequestUpdate` | log; on UpdateRequest, post SM event via TIPC |
| **PER** | no (it's the backend, not a consumer) | `PersistencyIf::Read/Write` | in-memory key→value map; future: file-backed |

### F — wiring (theia)

1. `services.manifest.service` adds each FC's bazel_target +
   start_cmd override.
2. `artheia executor emit --machine central_host demo.manifest.rig`
   shows all four under `core_sup`'s children.
3. `bazel test //services/system/...` runs all four FCs' tests.

## Out of scope (deferred)

- DM and NM (sm.md inbound arrows — they post events TO sm rather
  than receiving SmStateStream; need their own design pass).
- PHM (EM→PHM crash report) — needs EM to actually spawn processes
  first.
- Cross-process TIPC subscription wiring. All four binaries today
  are in-process-only; partners get the broadcast through stand-in
  callbacks in tests. The actual TIPC publisher / subscriber on
  SmStateStream is a separate work item.
- Bazel-driven nanopb regen. Today the .pb.{c,h} are committed.
- gen-cpp-stubs integration. The `_gen.h` it emits today is a C-API
  facade that doesn't match GenServer's shape; integration is its
  own design pass.

## Survey notes (already done)

- 18 TIPC IDs across `services/system/*/package.art` are all
  distinct in range `0x80010001..0x80010012`. No collisions.
- Only sm has a binary today. All other FCs are .art-only.
- gen-proto exists, works, has stable output shape.
- `platform/proto/gateway/system/` is a working precedent for the
  per-package proto layout.
- nanopb is in MODULE.bazel (0.4.9.1). Existing cc_library
  consumers link via `-l:libprotobuf-nanopb.a`.

## Decision log

- **Proto path layout**: `platform/proto/system/services/<fc>/` —
  mirrors the .art package hierarchy, not the source-tree location.
  This means the include path is
  `#include "system/services/<fc>/<Msg>.pb.h"` (with C++ namespace
  `services::services::<fc>::<Msg>` per the leading-segment
  rewrite).
- **daemon.sh strategy**: drop, don't redirect. Build-time warning
  for FCs with no start_cmd is the honest signal.
- **Hand-rolled POD lifetime**: gone once Phase B lands. Going
  forward, every FC consumes proto-emitted headers.

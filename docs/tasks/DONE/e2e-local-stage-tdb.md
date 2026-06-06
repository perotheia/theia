# E2E local-stage + tdb integration test

> **DONE (2026-06-04).** Supervisor S1–S5 landed (boot from staged
> executor.json, fork all children, clean shutdown), GetTree/GetChild return
> populated snapshots, and the tdb client drives the live tree (`ps`, `info`,
> `loglevel`, `trace`, `logcat`) over the .art-declared probe. `theia
> stage-local` + Puppet local_install stages the binaries; demo `stage_local.sh`
> emits the cluster netgraph.json. Residual `logcat -g/-c` (ring size/clear) is
> a minor follow-up tracked inline, not blocking.

Goal: a runnable end-to-end smoke — stage the current build into `install/`,
boot the new `ara::exec` supervisor from `executor.json`, then drive + observe
it with `tdb` (not com/supdbg): list the tree, flip trace on a component,
subscribe to that trace via `tdb → log[trace]`.

## Plan under review (user's 7 steps) + verdict

1. **Fix `demo/stage_local.sh` to install the current build into `install/`,
   removing stale installed files.** ✅ in scope, but the script is badly stale
   (see Delta S1). Needs more than "remove old files".
2. **Generate the manifest from `system/services/cluster.art` → `services/manifest/`,
   and REMOVE manifest generation from gen-app.** ✅ correct, and HALF-DONE
   already: `artheia gen-manifest <system.art> services/manifest/service.py`
   already builds the FC list from `cluster Services` + sidecars the
   hand-written supervisor tree in `executor.py`. The bug to fix is narrower
   than stated — see Delta S2.
3. **Generate netgraph.** ✅ `artheia gen-netgraph` exists and stage_local
   already calls it; the only gap is the binary path + that no FC consumes it
   at runtime yet (cosmetic for this test). See Delta S3.
4. **Serialize rig.py manifests into `install/`; split rig.py → one Machine,
   copy to `zonal_rig.py` (central+compute), drop compute from rig.py.** ✅
   reasonable; rig.py today is 2-machine (central_host + compute_host) + an
   admin host. See Delta S4.
5. **Run stage_local.sh; start supervisor with `install/<m>/executor.json`.** ✅
   the new supervisor reads `THEIA_SUPERVISOR_MANIFEST` (env) → `load_manifest`.
   `executor.json` is produced by `artheia executor emit --rig <R> --machine <m>`.
   See Delta S5.
6. **Drop `tools/supdbg`; use `tdb` to inspect the supervisor; add tdb commands
   as needed.** ⚠️ **BIGGEST GAP: `tdb` DOES NOT EXIST.** It is only a DESIGN
   in docs (composition-isolation-test.md §"tdb — the Theia Debug Bridge").
   supdbg (tools/supdbg, gRPC-to-com) is what exists. See Delta S6 — this is
   net-new tool work, not a swap.
7. **Use tdb to trigger trace on a component + subscribe `tdb → log[trace]`.** ✅
   the wire path exists (supervisor ConfigureTrace → child Tracer;
   log[trace] TraceCtl Subscribe firehose — both built + tested by
   observer_stream.py). tdb just needs to be the client. See Delta S7.

## What's MISSING for the e2e (the real blockers)

- **B1 — `tdb` is vapor.** No tool. rf currently reaches the supervisor via
  **gRPC → services/com SupervisorView** (testing/rf_theia/adapters/
  supervisor_grpc.py wraps tools.supdbg.client, endpoint localhost:5051). com
  is RETIRED. So there is NO working supervisor client on the new all-TIPC
  path. tdb must be built (or the existing `artheia.observer` / a TIPC client
  extended) before steps 6–7 can run.

- **B2 — supervisor control client over TIPC.** ✅ RESOLVED + PROVEN LIVE.
  `tools/tdb/system/tdb.art` declares two client nodes (TdbSup / TdbTrace) that
  `import system.supervisor.* / system.services.log.*` and reference the real
  interfaces (FQN-qualified to dodge the SupervisorControlIf forward-decl that
  also lives in the log package). `artheia.probe` resolves SupervisorCtl
  (0x80020001) + all 11 ops + TraceCtl (0x80010014). `tools/tdb/tdb_client.py`
  is the probe-backed client (SupervisorClient.get_tree/configure_trace/... +
  TraceClient reusing artheia.observer). NO raw TIPC — transport-swap-safe.
  Probe enhancement landed: ArtheiaContext now follows `extern` forward-decls
  through `import` lines to the real node def (was: extern nodes skipped, so a
  client .art couldn't address a peer it doesn't define).
  PROVEN LIVE over real TIPC (`modprobe tipc` — the host has it; the earlier
  "no AF_TIPC" was just an unloaded module): against a booted supervisor,
  tdb_client did GetSystemInfo (got real host facts), ConfigureTrace
  (status=0), and GetTraceConfig (read the stored config back) — full nanopb
  round-trip incl. readable string fields. See feedback-clients-via-art-probe.

- **B3 — GetTree returns an EMPTY envelope.** The port made GetTree/GetChild
  return empty (the monolithic children[] retired with com); the live tree is
  the NodeEdge/NodeState firehose on the `events` senders. So `tdb ps` /
  `tdb supervisor` must consume the FIREHOSE + reassemble (the name-keyed
  reassembler logic that lived in com), NOT call GetTree. Either re-home the
  reassembler into tdb, or re-populate GetTree.children server-side.

- **B4 — stage_local.sh is stale on every path:** CMake supervisor path
  (`bazel-bin/platform/supervisor/supervisor/bin/supervisor` → now
  `//platform/supervisor/main:supervisor`), CMake log build
  (`services/log/build/services-log` → now `//services/log/main:log`),
  `demo.manifest.rig` ref, 2-machine assumption, no executor.json env wiring
  (supervisor reads THEIA_SUPERVISOR_MANIFEST, not argv).

- **B5 — manifest → executor.json shape.** `load_manifest` (spec.cpp) expects
  a JSON supervisor tree (name/strategy/children/start_cmd/nodes...). Confirm
  `artheia executor emit` emits THAT shape (it emits the sliced supervisor
  tree; field-match against spec.cpp's loader, esp. the `nodes[]` block with
  tipc_type/tipc_instance/reporting that trace-push needs).

- **B6 — trace adapter feeds off stderr text, not the firehose.** rf's
  tracer_jsonl adapter tails the `TRC v1 ...` stderr lines; the new trace
  egress is BINARY records over TIPC Subscribe (observer.py / observer_stream.py
  is the working reader). tdb trace-subscribe must use the observer path.

## Delta to implement (ordered; S = step)

- **S1 (stage_local.sh rewrite).** Repoint to bazel targets:
  `//platform/supervisor/main:supervisor`, `//services/<fc>/main:<fc>`,
  `//services/log/main:log`. Drop CMake refs. `rm -rf install/<m>` then stage.
  Write `executor.json` via `artheia executor emit --rig <R> --machine <m>`.
  Launch wires `THEIA_SUPERVISOR_MANIFEST=install/<m>/executor.json` +
  `THEIA_ROOT_DIR=install/<m>`.

- **S2 (manifest cleanup).** Remove `--manifest-out` from gen-app
  (cli.py:1145/1205 + fc_app.py:605/manifest_out param — already a no-op/deprec,
  just delete the option + plumbing). Document `gen-manifest <system.art> services/manifest/service.py` as THE manifest generator (cluster.art-driven +
  executor.py sidecar). No new generator needed.

- **S3 (netgraph).** Keep `artheia gen-netgraph -R system/system.art --out install/central/netgraph.json` in stage_local (log[trace] reads it for the
  addr→name rewrite). No runtime FC consumes the per-node netgraph headers yet
  (latent: hardcoded sm-gate 0x8001001D in send_sm_ready could become a
  `connect` — deferred, platform-wide).

- **S4 (rig split).** `rig.py` → single Machine (central only); copy to
  `zonal_rig.py` with central+compute; drop compute_host from rig.py. Update
  stage_local to stage 1 machine from rig.py (zonal_rig.py for the 2-box run).

- **S5 (boot).** After stage, `cd install/central && THEIA_SUPERVISOR_MANIFEST= ./executor.json ./supervisor`. Verify children fork (the smoke already works
  with a hand JSON tree).

- **S6 (tdb — NET NEW).** Build `tdb` per the design (adb-shaped: ps / attach /
  trace / logcat / supervisor). It wraps a TIPC SupervisorCtl client + the
  firehose reassembler (B3) + the observer trace reader (B6). Drop tools/supdbg

  + its gRPC client AFTER tdb covers `Get Supervisor Tree` / trace for rf.
    Repoint testing/rf_theia/adapters/supervisor_grpc.py → a tdb/TIPC adapter.

- **S7 (trace e2e).** `tdb trace <node>` → SupervisorCtl.ConfigureTrace →
  child Tracer enables; `tdb logcat` / attach → Subscribe to log[trace]
  TraceCtl → binary firehose → decode. observer_stream.py already proves the
  wire; tdb is the CLI front-end.

## STATUS — S1–S5 DONE (booting supervisor from a staged install/)

Landed + verified end-to-end:

- **S2** ✅ gen-app `--manifest-out` removed; manifest is gen-manifest
  (cluster.art-driven). artheia commit.
- **S4** ✅ rig.py → single-machine (central); zonal_rig.py = the 2-machine
  spec; multi-host tests repoint to zonal.
- **S1** ✅ stage_local.sh rewritten to bazel targets + executor.json + env;
  platform required, demo apps best-effort.
- **S5** ✅ FULL LIVE SYSTEM RUN over real TIPC. `stage_local.sh` →
  install/central (clean, all 7 binaries); the supervisor boots executor.json,
  forks all 7 children, every FC binds its TIPC socket (sm 0x8001000D / sm_gate
  0x8001001D / per 0x80010007 / ucm 0x8001000E / shwa 0x80010012), the demo
  apps run real cross-node traffic (DriverNode casts Inc 10×, CounterNode
  handle_call(Get), ObserverNode polls 0→84, TickerNode 10 ticks), then SIGTERM
  → clean reverse-order shutdown of all 7, rc=0. ZERO exec failures / abnormal
  exits. The whole manifest→stage→boot→fork→run→shutdown pipeline works on the
  new ara::exec architecture.
- **tdb LIVE** ✅ against the booted supervisor: GetSystemInfo (real host
  facts), ConfigureTrace (status=0), GetTraceConfig (read the stored config
  back) — full nanopb round-trip over TIPC via the probe client.
- **B5** ✅ `artheia executor emit` shape matches load_manifest (spec.cpp)
  exactly (name/strategy/children/start_cmd/.../nodes[]).
- **log trace-cut** ✅ (unblocked the stage) — log[trace] is subscription-only;
  TraceControl/TraceConfigRequest removed from .art + proto + lib + impl + main;
  //services/log/main builds green (was pre-broken).

(The Demo3WayP{1,2,3} apps build + run fine — the earlier "runtime API drift"
was a misdiagnosis: they were just missing the DEMO\_→THEIA_DECLARE_REMOTE_CODEC
rename in demo/*/lib/demo_codecs.hh, which poisoned the TU parse. Fixed.)

- **S6 (tdb CLI)** ✅ MOSTLY DONE. tools/tdb/tdb.py — adb-shaped, two modes
  (one-shot argv + prompt_toolkit REPL). Verbs: ps / supervisor / trace /
  trace-config / restart / terminate / logcat. Probe-backed. Verified LIVE over
  TIPC: ps renders the tree, supervisor shows host facts, trace/trace-config
  round-trip, terminate stop-and-holds.
- **B3 (GetTree empty)** ✅ RESOLVED server-side: Supervisor::ctl_get_tree()
  walks the tree into TreeSnapshot.children (no firehose reassembler needed for
  the snapshot path). `tdb ps` consumes it directly.

REMAINING for the FULL e2e: (S7) `tdb logcat` end-to-end — the TraceClient
(artheia.observer) path is wired but needs a live `tdb trace X` → records
flowing test against a real reporting FC (observer_stream proves the wire).
Then: drop tools/supdbg (tdb supersedes it); wire `logcat -g/-c` (ring
size/clear). All runnable on this host (`modprobe tipc`).

## Sequencing reality

The 7 steps are NOT independent. Hard dependency chain:
S1/S4 (stage) → S5 (boot) → **S6 (build tdb — the long pole)** → S7 (trace).
B1–B3 (no tdb, no TIPC supervisor client, empty GetTree) are the gating work;
steps 1–5 are mostly stale-script repair + already-existing generators. The
e2e cannot pass until tdb (or an equivalent TIPC client + firehose reassembler)
exists. Recommend: land S1–S5 first (gets a booting supervisor + staged tree),
then scope tdb (S6) as its own effort, then S7 falls out.
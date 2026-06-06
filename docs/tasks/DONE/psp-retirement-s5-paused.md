# S5 paused — `node runnable` was wrong; re-think the .art shape first

> **DESCOPED (2026-06-04).** psp retirement is considered done at S4. S5/S6
> (the odd_path_monitor rewire captured below) are no longer in scope; kept
> here as a record of where the `.art` shape investigation stopped, should the
> odd-path work ever be picked up independently.

Subordinate to [psp-retirement.md](psp-retirement.md). S5 began by
re-doing the `.art` spec for `odd_path_monitor` so the generator could
emit typed handlers. That hit a chain of issues; the user pulled
the cord on the `node runnable` direction. **Captured here so the
next session resumes from facts, not vibes.**

## What happened

1. Tried to **rebind the 18 PSP interfaces** in the app's package.art
   with `data system.autosar.mlbevo_gen2.fibex.<Pdu> record` lines.
   **Failed:** textX cross-ref resolution happens at parse time
   BEFORE `_resolve_forward_decls` chases imports, so the imported
   PSP messages aren't visible to `data <FQN>` cross-refs. Error:
   `Unknown object "system.autosar.mlbevo_gen2.fibex.EML_01" of class "MessageOrEnum"`.

2. Investigated the wire/runtime reality and found a real impedance:
   `TipcMux` dispatches **only** `GW_BUS_TYPE_RPC` frames
   (`GW_MSG_GEN_CAST` / `GW_MSG_GEN_CALL`) keyed on
   `GwRpcMeta.service_id`. **Signal updates**
   (`GW_BUS_TYPE_FLEXRAY` / `GW_BUS_TYPE_CAN` with
   `GW_MSG_SIGNAL_UPDATE`) ride the same TIPC socket but are dropped
   by `TipcMux` — their dispatch key is `flexray.slot_id` (or
   `can.can_id`), not `service_id`. A vanilla `GenServer` therefore
   **cannot** receive signal updates over its mailbox today.

3. Proposed `node runnable FlexRayIngress` (a `GenRunnable` —
   "worker that owns a thread, ignores the mailbox") as a zero-
   runtime-change fit: `do_loop()` runs `GwClient::recv_signal()`,
   dispatches by `slot_id`, hands typed structs to
   `FlexRayCmpAdapter`. **The user rejected this** ("wrong"). Pausing
   for a re-think before any more code changes.

## What's on disk right now

- **pero_theia, branch `psp-retirement` (HEAD `8cd9c47`):** clean
  through S4. S5's wrong-direction work didn't touch this repo.
- **artheia, branch `gen-app-lib-kind` (HEAD `6f0a849`):** clean
  through S4 (the `--kind lib` generator). S5 didn't change artheia.
- **vendor/odd_path_monitor, branch `flexraya_world_source`:**
  - `platform/` deleted via `git rm -rf platform` (S3 cleanup,
    not yet committed).
  - `system/autosar` → `../../../system/autosar` symlink added
    (S3 FQN mirror, not yet committed).
  - `system/odd_path_monitor/{package,component}.art` written
    (S3 spec, not yet committed).
  - **`platform/` re-emitted by S4's `gen-app --kind lib`** —
    builds clean on host (libplatform_runtime.a + protos + impl).
  - **`package.art` re-edited by S5 with the bad `*_Sub` rebind**
    that doesn't parse. The S3-known-good version is in this commit's
    DELTA below — see "Revert before resuming".
  - **Submodules under `3pp/` now checked out** (BehaviorTree.CPP,
    Groot1Publisher, RTKLIB, ad-rss-lib, msgpack-cxx). Not built,
    just cloned. The full app still won't link without `3pp/install/`
    (per the existing `docs/rss-bt4-build.md`).

## Revert before resuming

`vendor/odd_path_monitor/system/odd_path_monitor/package.art` should
go back to the S3-known-good form (empty `_Iface` forward-decls). The
right way is:

```sh
cd vendor/odd_path_monitor
# Drop the *_Sub experiment, restore the empty-stub *_Iface form.
git checkout HEAD~  -- ../../  # NO — see below; the .art isn't committed.
```

Actually the S3 .art was **never committed in the vendor sub-repo**
(it sits in the working tree on `flexraya_world_source`). Reconstruct
from the version committed in pero_theia's psp-retirement S3 commit:

```sh
git -C ~/repo/theia show 9115262:- | …  # NO — the file lives in vendor sub-repo, not pero_theia.
```

The S3 spec is reconstructable from the descriptions in
[psp-retirement.md](psp-retirement.md) §"S3 outcome" — package.art
declares 18 empty `interface senderReceiver <Pdu>_Iface { }` +
forward-decl `node atomic MlbevoGen2_Bus` + the FlexRayIngress node
with 18 receiver ports. Or restore from one of the backups under
`~/up/`.

## The dimensions S5 has to actually pick from

The user's stated goal: `GenServer` receives casts from the gateway,
no runtime rewrite, runnable was wrong. The constraints:

| dimension | option | implication |
|---|---|---|
| node base | `GenServer` (atomic) | mailbox-driven, RPC-shaped wire (today) |
|           | `GenStateM` | FSM-shaped, not our case |
|           | `GenRunnable` | worker-thread, REJECTED |
| signal-update delivery | bridge worker → `cast(self, ...)` | adds a hop, doubles wire bytes, no runtime change |
|                        | extend TipcMux | "rewrite platform/runtime" — the user explicitly said no |
|                        | something else | TBD — needs design discussion |

The remaining shape that fits "GenServer receives casts" without
rewriting the runtime: a **bridge worker inside `do_start()`** that
runs `GwClient::recv_signal()` and casts each decoded PDU into
`FlexRayIngress` itself (TIPC self-cast goes through TipcMux's RPC
path; arrives at `handle_cast(const EML_01&, State&)`). But:

- The bridge worker's nanopb-decode needs the **18 PSP `.pb.h` files**
  visible to the app. Vendoring them into `<app>/platform/generated/`
  is the lift; the existing PSP genrule lives in Bazel land.
- The `cast()` needs **typed RemoteCodec specializations** for each
  PDU, which means the **codec gap from S4 is still in scope** —
  the app's package.art still has to make 18 PSP messages cross-
  referenceable so the generator emits codecs for them.

So the textX cross-ref issue from step 1 (this paused work) is the
**actual blocker** regardless of node-base choice. The next session
must either:

- **Pre-resolve the imports** before textX parses (a parser-side
  change in artheia — read the imported files, merge their model
  elements into the entry's model before cross-ref resolution).
- **Locally re-declare the 18 PSP messages** in the app's package.art
  (drift-prone; the wire-id is content-hashed on type-NAME, so
  `RemoteCodec<system_odd_path_monitor_EML_01>` and
  `RemoteCodec<mlbevo_gen2_fibex_EML_01>` would NOT match — the
  gateway's wire `service_id` wouldn't reach this app's mailbox).
- **Co-opt the existing PSP `<Pdu>.pb.h`** by hand-rolling the codec
  declarations in the impl/ layer, sidestepping the generator. Drops
  the typed-handle_cast benefit but keeps S5 unblocked.

## Suggested next-session approach

Read this note + the S2 integration map first. Then:

1. Pick one of the three sub-options above (probably option 3 —
   sidestep the generator for the codec hookup, since this is one
   app and option 1 is a parser change deserving its own task).
2. Restore the S3-good `.art` (drop my `*_Sub` rebind).
3. Decide the bridge-worker shape.
4. Write the `FlexRayIngress::handle_cast` bodies.
5. Wire `app/core/world/FlexRayWorldSource.cpp` to construct
   `FlexRayIngress`, inject its FrameSink → adapter, start the node.
6. `cmake --build` and accept the unrelated 3pp/install link failure.

The submodules are now cloned, but `3pp/install/` (built ad-rss-lib +
BehaviorTree + Groot1Publisher artifacts) isn't. Per
`docs/rss-bt4-build.md`. Not S5's job to set up.

## Lessons from this aborted attempt

- **textX cross-ref scoping is a real platform limitation, not a
  bug.** Imports register a forward-decl-substitution source for
  Cluster/Composition stubs only; messages, enums, interfaces aren't
  followed. The `_resolve_forward_decls` post-pass runs AFTER textX
  has already failed cross-refs to imported types.
- **Don't propose `GenRunnable` for "consumer of an event stream"
  shape** when the user has framed it as `GenServer receives casts`.
  Even though `GenRunnable` is a clean technical fit, the user's
  framing is the architectural commitment.
- **Investigate-and-report > rewrite.** The user's directive on this
  task ("we was trough this bullshit already 2 times when you tried
  to rewrite platform runtime and miserably failed") is now in
  [[feedback-verify-head-before-commit]]-adjacent territory. Keep
  it in mind on every platform-runtime-touching task.

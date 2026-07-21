# `gen-fc`: the lib/main/impl split

How an FC's C++ is generated from its `.art` spec. Generator:
`artheia/artheia/generators/fc_app.py`; templates in
`artheia/artheia/generators/templates/fc_app/`.

## The command

```sh
PATH="$PWD/.venv/bin:$PATH" artheia gen-fc \
    services/<fc>/system/<fc>/package.art \
    --out        services/<fc>/ \
    --proto-out  platform/proto/ \
    --ns         ara::<fc> \
    --force
```

| flag | meaning |
| --- | --- |
| `gen-fc` | FC mode (vs `psp`). |
| `<art_file>` | the spec — `services/<fc>/system/<fc>/package.art` (loader merges the sibling `component.art`). |
| `--out` | the impl tree root, `services/<fc>/`. |
| `--proto-out` | where the `.proto` lands — `platform/proto/`. The committed `.proto` is the source; `.pb.*` are gitignored, genrule-derived. |
| `--ns` | C++ namespace, e.g. `ara::log`. Defaults to the package FQN flattened with underscores. |
| `--force` | overwrite the **first-time-only** files (impl handlers + `impl/BUILD.bazel`). Without it they're written once and then skipped. |

## What lands where

```
services/<fc>/
  lib/                       ── AUTO-GENERATED, DO NOT EDIT (rewritten every run)
    <Node>.hh                  the GenServer/GenStateM/GenRunnable subclass
    <Node>_netgraph.hh         signal→destination projection for this node
    <fc>_codecs.hh             FC-wide inbound RemoteCodec specializations (deduped)
    Log.hh                     per-FC logging context
    BUILD.bazel
  main/                      ── AUTO-GENERATED, DO NOT EDIT (rewritten every run)
    main.cc                    entrypoint: constructs nodes, joins the TipcMux
    BUILD.bazel
  impl/                      ── HAND-OWNED (first-time-only; --force to overwrite)
    <Node>_handlers.cc         your handler bodies (the only file you edit)
    BUILD.bazel                first-time-only, but IS checked by regen-stability
```

The split: `lib/` + `main/` are a pure projection of the `.art` — rewritten
verbatim every run. `impl/<Node>_handlers.cc` carries your business logic;
gen-fc refuses to overwrite it unless `--force`. It's the *only* file you
hand-edit. To change anything in `lib/`/`main/`, change the `.art` (or the
Jinja template) and regenerate — never edit the generated file.

## One node = one skeleton; multi-node FCs decompose

A single FC package may declare more than one node. gen-fc emits per-node
`<Node>.hh`, `<Node>_netgraph.hh`, plus the **write-once** pair
`impl/<Node>_handlers.cc` (handler bodies) and `impl/<Node>_state.hh` (the
node's state struct), with shared `<fc>_codecs.hh` + `Log.hh`. So
`services/log` (three nodes: `LogDaemon`, `TraceStreamPump`, `TraceCtl`)
produces:

```
lib/LogDaemon.hh        lib/LogDaemon_netgraph.hh
lib/TraceStreamPump.hh  lib/TraceStreamPump_netgraph.hh
lib/TraceCtl.hh         lib/TraceCtl_netgraph.hh
lib/log_codecs.hh       lib/Log.hh
impl/LogDaemon_handlers.cc   impl/TraceStreamPump_handlers.cc   impl/TraceCtl_handlers.cc
impl/LogDaemon_state.hh      impl/TraceStreamPump_state.hh      impl/TraceCtl_state.hh
```

Regenerating one node never disturbs a sibling's hand-written handlers or
state. Both `impl/<Node>_handlers.cc` and `impl/<Node>_state.hh` are
APP-OWNED and never overwritten unless you pass `--force` (which clobbers
both).

## Which base class gen-fc picks

Per node, by shape of the `.art` `node` declaration:

| `.art` node | base class | handler contract |
| --- | --- | --- |
| `node atomic` (no statem) | `GenServer` | `handle_cast(Msg, State)`, `handle_call(Req, State) → Reply` |
| `node atomic` + `statem { … }` | `GenStateM` | per-state `handle_event(State, Event, …)` |
| `node runnable` | `GenRunnable` | `do_start()`, `do_loop()`, `do_stop()` |

Selection logic (`fc_app.py`): `runnable` wins, else `statem`, else plain
GenServer — choosing the `Daemon[.statem|.runnable].hh.j2` +
`handlers[.statem|.runnable].cc.j2` template pair.

## The regen-stability guard

`testing/rf_theia/scenarios/_selftest/fc_regen_stability/` enforces the
contract: **committed `lib/`, `main/`, and `impl/BUILD.bazel` must equal
gen-fc's output byte-for-byte.** Only `impl/<Node>_handlers.cc` is exempt
(detected by its `HAND-OWNED` / `FIRST-TIME-ONLY SCAFFOLD` banner). The
`FC_SPECS` table in `fc_regen_lib.py` maps each short → spec path → namespace:

```python
FC_SPECS = [
    ("sm",  "services/sm/system/sm/package.art",   "ara::sm"),
    ("com", "services/com/system/com/package.art", "ara::com"),
    ("per", "services/per/system/per/package.art", "ara::per"),
    ("ucm", "services/ucm/system/ucm/package.art", "ara::ucm"),
    ("log", "services/log/system/log/package.art", "ara::log"),
]
```

Run it:

```sh
cd testing && PATH="$PWD/.venv/bin:$PATH" \
  python -m robot rf_theia/scenarios/_selftest/fc_regen_stability/fc_regen_stability.robot
```

If a test fails, the fix is **not** to commit a hand-edit to a generated
file. Either change the `.art` and re-run gen-fc, or — if the template is
wrong — fix the template under `templates/fc_app/` and regenerate every FC.
When you add a field/node to a spec, regenerate, then build to confirm:

```sh
bazel build --config=linux //services/<fc>/main:<fc>
```

## `gen-fc-lib`: the reusable-unit (no-main) mode

`gen-fc` builds a runnable daemon (lib + **main** + impl). `gen-fc-lib`
builds a **linkable unit with NO main** — the ROS-package shape a workspace
imports and links. It is how `theia init --kind package` scaffolds are built (see
the SKILL's "Packages" section for the two-package repo layout).

```sh
# from a package repo, source .art under system/<X>/, codegen to src/:
artheia gen-fc-lib system/<X>/package.art \
  --out src --proto-out proto --ns ara::<X>
```

| flag | effect (vs `gen-fc`) |
| --- | --- |
| `gen-fc-lib` | emit `lib/` + `impl/` + a **self-contained proto** (nanopb genrule + `<X>_proto` cc_library), **NO `main/`**. |
| `--out src` | codegen tree. Lib → `src/lib` (regen, gitignored); impl → `src/impl` (**write-once**, tracked). Keep it OUT of the hand-edited `system/` source. |
| `--ns ara::<X>` | the package's C++ namespace — required, else co-composed packages collide on `Log.hh`. |
| `--proto-out proto` | the package's proto lands at `proto/system/<X>/` (keyed off the `.art` **FQN**, not `--out`); label `//proto/system/<X>:<X>_proto`. |

What lands where — the labels a consuming composition derives from
`import system.<X>.*` (all resolved in `fc_app.py`
`_imported_node_lib` / `_imported_package_impl_deps` /
`_imported_package_proto_deps`):

- `//src/lib:<X>_lib` — the node class + wiring (regenerated every run; the proto
  dep is auto-correct).
- `//src/impl:<X>_impl` + `//src/impl:<X>_state` — handler bodies + state structs.
  **Write-once**: the gen-fc family refuses to overwrite `src/impl/*` and its
  `BUILD.bazel` without `--force` — so a custom algo target (e.g. v2v's
  `v2v_algo`) or a `select()` survives regen. **`--force` CLOBBERS the write-once
  impl** — back it up first if you must refresh lib+proto that way; plain regen
  (no `--force`) skips impl.
- `//proto/system/<X>:<X>_proto` — the package's own messages (a consuming SWP
  links just these, not the framework `//platform/proto` aggregate).

A `gen-fc` run on a `component.art` that `import`s the package then
LINKS these prebuilt targets and does **not** regenerate the imported node
(filtered in the per-node loop + `BUILD.lib/impl.j2`). Externally (package cloned
into another workspace) the same node resolves as `//packages/<X>/src/lib:<X>_lib`.

Verify a fresh package end to end:

```sh
bazel build //apps/<Cls>Tester/main:<X>_tester   # the in-repo tester links the package
```

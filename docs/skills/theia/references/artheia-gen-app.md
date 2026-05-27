# `gen-app --kind fc`: the lib/main/impl split

How an FC's C++ is generated from its `.art` spec. Generator:
`artheia/artheia/generators/fc_app.py`; templates in
`artheia/artheia/generators/templates/fc_app/`.

## The command

```sh
PATH="$PWD/.venv/bin:$PATH" artheia gen-app --kind fc \
    services/<fc>/system/<fc>/package.art \
    --out        services/<fc>/ \
    --proto-out  platform/proto/ \
    --ns         ara::<fc> \
    --force
```

| flag | meaning |
| --- | --- |
| `--kind fc` | FC mode (vs `psp`). |
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
gen-app refuses to overwrite it unless `--force`. It's the *only* file you
hand-edit. To change anything in `lib/`/`main/`, change the `.art` (or the
Jinja template) and regenerate — never edit the generated file.

## One node = one skeleton; multi-node FCs decompose

A single FC package may declare more than one node. gen-app emits per-node
`<Node>.hh`, `<Node>_netgraph.hh`, and `<Node>_handlers.cc`, with shared
`<fc>_codecs.hh` + `Log.hh`. So `services/log` (two nodes) produces:

```
lib/LogDaemon.hh   lib/LogDaemon_netgraph.hh
lib/TraceCollector.hh   lib/TraceCollector_netgraph.hh
lib/log_codecs.hh   lib/Log.hh
impl/LogDaemon_handlers.cc   impl/TraceCollector_handlers.cc
```

Regenerating one node never disturbs a sibling's hand-written handlers.

## Which base class gen-app picks

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
gen-app's output byte-for-byte.** Only `impl/<Node>_handlers.cc` is exempt
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
file. Either change the `.art` and re-run gen-app, or — if the template is
wrong — fix the template under `templates/fc_app/` and regenerate every FC.
When you add a field/node to a spec, regenerate, then build to confirm:

```sh
bazel build --config=linux //services/<fc>/main:<fc>
```

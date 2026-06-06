# rf-theia v3 — augment with artheia.probe

Revisit of `testing_framework_v3.md` now that **`artheia.gen_server.probe`**
exists (built 2026-06; see `project-probe-design` memory). The probe binds any
node's TIPC address from the parsed `.art` and speaks the real gen_server wire
(cast / call / call-reply), in both directions (drive an FC AND mock a peer it
talks to). That directly subsumes the hand-rolled TIPC machinery v3's pairs
would otherwise each reinvent.

## The boundary question — resolve it first

v3 decision: *"rf-theia reads artheia's stable OUTPUTS (`rig.json`,
`netgraph.json`, manifest) but never imports `artheia.*`."* The probe lives at
`artheia.gen_server.probe` — importing it crosses that line.

**Resolution: the probe is an OUTPUT, not the toolchain.** The boundary's intent
was "don't entangle the test harness with the SUT *parser/codegen*" (v2's
mistake). The probe is neither — it's a thin transport client built FROM the
`.art` (the same stable contract as rig.json/netgraph.json), with no parser/
generator coupling. Treat `artheia.gen_server.probe` as a fourth stable artheia
contract (alongside rig/netgraph/manifest JSON). rf-theia may import it; it must
NOT import `artheia.model` / `artheia.generators`. (Optionally: re-export the
probe as `artheia.probe` so the import reads as a public client API, not an
internal path.)

## What the probe gives each v3 pair

| Pair | v3 plan | What the probe replaces / augments |
| --- | --- | --- |
| **4 Distributed actors** (`components.py`) | hand-rolled `PortSpec kind="tipc"` — "TIPC SEQPACKET into the cluster" + a Component/transport abstraction | **The probe IS this.** `ctx.probe(node)` binds the address, `cast/call` send, `on_cast/on_call/serve` receive + mock. Delete the bespoke TIPC port code; a Component's tipc ports become probe handles. `serve()` makes a Component a full FC mock; `probe_external()` binds a tester identity not in the `.art`. |
| **2 Temporal logic** (`monitors.py`) | `Assert Eventually/Always/Never` over trace events | The probe gives a SECOND event source besides the trace stream: `await_cast(msg)` / `arm_cast` turn "a node sent/received message M" into a first-class awaitable the monitors assert on — reactive, not trace-scraping. |
| **1 Hybrid automata** (`flow_engine.py`) | FSM runner; `Emit Event crash` etc. | The probe is the event INJECTOR + observer: drive `RestartChild` via `call(SupervisorCtl, ...)`, observe the FSM's casts via `await_cast` — the flow's `Emit Event` / `Wait For State` become probe sends / awaits, no separate adapter. |
| **3 Supervision graph** (`supervision.py`) | strategy-aware assertions | Augment, don't replace: supervision reads executor.yaml for the tree; the probe drives the supervisor control surface (`RestartChild`/`TerminateChild`) + observes the NodeEdge/NodeState firehose via `on_cast` to assert restart ORDER live. |
| **5 Topology** (`topology.py`) | reads netgraph.json — static reachability | Augment: topology answers "X *should* reach Y" (static graph); the probe answers "X *can* reach Y *right now*" (live: actually connect + call). `Route Exists` (static) + a new `Route Live` (probe connects). |

## Net effect

- **Pair 4 collapses** into the probe — the `components.py` transport layer was
  re-implementing the probe. Biggest win.
- **Pairs 1–3** gain a typed, reactive event channel (probe casts/calls) beside
  the trace stream — assertions on "node sent/received M" without scraping
  trace records.
- **Pair 5** gains a *liveness* check (static graph says reachable; probe proves
  it).

The probe doesn't replace the v3 architecture — it's the **transport substrate**
under pairs 1–4 and a liveness oracle for pair 5. Robot keywords + Pydantic rig
context stay exactly as designed.

## Plan (iteration order, test-first per v3)

0. **Boundary commit** — declare the probe a stable artheia contract; add it to
   rf-theia's allowed imports (with the model/generators ban explicit). Optional
   `artheia.probe` re-export.
1. **Adapter: `runtime/probe_adapter.py`** — wrap `ctx.probe(...)` with the rig
   context (resolve node names from rig.json/the .art the rig points at), retry
   on stale TIPC co-bindings (the known gotcha), and a clean `send/await/serve`
   surface the keywords call. ~one module.
2. **Pair 4 cut-over** — re-point `components.py`'s `kind="tipc"` PortSpec at the
   probe adapter; delete the bespoke TIPC client. The first concurrent-tester
   scenario (gateway-tester + observer-tester) drives it. Existing Component /
   Robot surface unchanged.
3. **Pair 2 event source** — `monitors.py` gains a probe-backed predicate:
   `Assert Eventually  probe.cast(M) on N  within=5s`. First test: a config push
   (ConfigUpdated) lands on a watcher within budget — already probe-provable.
4. **Pair 1 / 3 augment** — flow `Emit Event` / supervision restart-order
   assertions route through the probe adapter where they need to inject or
   observe a typed message (vs the trace stream).
5. **Pair 5 liveness** — add `Route Live` (probe connects + calls) beside the
   static `Route Exists`.

Each step is test-first (write the Robot scenario, implement just enough). The
self-tests stay green; the v1→v3 keyword-hiding plan is unaffected.

## Risks / notes

- **Stale TIPC co-bindings** confound probe tests (leftover FC instances load-
  balance the name connect). The adapter must retry on a fresh socket within a
  budget (the probe already does; surface it). See `project-probe-connect-stale-bindings`.
- The probe needs `proto_root` (platform/proto) — the rig context already knows
  the workspace root, so the adapter wires it.
- Keep the probe import quarantined to `runtime/probe_adapter.py` so the
  SUT/test boundary is one auditable file.

## Status

Not started — this is the plan. Probe exists + is proven (drives per, supervisor,
log live; server-side mocking). The cut-over is incremental, Pair 4 first.

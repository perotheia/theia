# Theia app trace → Robot Framework, via services/com (gRPC proxy)

Get a node's trace stream out of the cluster into rf-theia, controlled
end to end. Builds on the send-signal probe path (see
robot-node-in-svc-com.md) — same com gRPC front, opposite direction.

## Target e2e (what we want to prove)

1. **Enable.** rf asks the supervisor (via com) to turn on trace for the
   `sm` node. The supervisor sends a trace-config message to sm; sm
   starts emitting trace records to the trace collector.
2. **Stream.** rf asks the trace collector (via com) for the trace
   stream; com proxies the request to the collector and forwards records
   back to rf over gRPC.

```
rf → com gRPC (Configure trace: node=sm, on)
   → supervisor → NodeTraceCtl push → sm starts emitting
sm → TIPC TraceRecordSubmit → trace collector
rf → com gRPC (Subscribe trace stream)
   → collector TraceRecordStream → com → gRPC stream → rf
```

## What already exists (do NOT rebuild)

The pipeline is largely built across #354/#361/#386:

- **TraceCollector** (services/log, TIPC 0x80010013): `TraceControl`
  (config in), `TraceRecordSubmit` (every Tracer-equipped FC submits
  records here), `TraceRecordStream` (fans records to subscribers),
  `to_supervisor` client (trace config flows to the supervisor so it
  survives restart).
- **com TraceStream gRPC (#354):** `TraceStreamImpl` in
  services/com/src/main.cpp bridges the collector's stream out over the
  same gRPC port as SupervisorView; `rpc Subscribe → stream TraceRecord`
  in supervisor_bridge.proto. Backed by a `TipcTraceUplink` to the
  collector.
- **Trace enable (#361/#386):** supervisor pushes per-node trace config
  (NodeTraceCtl) to a reporting node; the node's Tracer gates emit.
- **rf-theia:** trace_decoder.py + tracer_jsonl.py adapters (the ctypes
  FFI path; google-protobuf decode is the going-forward replacement per
  the send-signal work).

So the remaining work is mostly **wiring + a Robot scenario**, not new
transport — UNLESS we take the architectural simplification below.

## DESIGN TENSION (the decision to make first)

Today the trace collector is a SEPARATE `services/log` daemon, and com
*proxies* to it. The user proposes the cleaner shape:

> Add the `log[trace]` collector as a NODE inside the `services/com`
> COMPOSITE, with the gRPC hook attached directly. Apps submit traces to
> `com[trace]`; it forwards to local TIPC subscribers (supdbg) OR remote
> (rf via gRPC). Simple and robust.

**ARA driver (decisive):** anything with an external port (gRPC) — and
firewalling — must be under com's control. The trace collector's stream
is externally bridged, so by ARA the external surface belongs in com.
Two ways to honor that:

- **(A) Relocate the collector into com's composite.** Make the trace
  collector a node in `services/com` `.art` (like the probe is now), with
  its own distinct TIPC address; com hosts the gRPC hook directly (no
  cross-daemon proxy). services/log shrinks to plain logging, or is
  retired for the trace role. Matches "simple and robust" + ARA: com
  owns the external port AND the collector. The multi-node com generator
  (ComDaemon + ProbeDaemon already) takes a third node cleanly.

- **(B) Keep services/log + the com proxy (status quo, #354).** com's
  TraceStream already bridges to the collector over TIPC. ARA is
  satisfied because the *external port* is in com; the collector stays a
  separate daemon reachable only over the internal TIPC bus. Less
  relocation, but com↔log is an extra hop and two daemons to supervise.

Recommendation: (A) for the ARA + simplicity reasons the user gives, and
because it parallels the probe (external-facing helper as a com node).
But (A) means moving TraceCollector's node + its submit/stream/control
ports into com and re-pointing every FC's `to_log`/trace-submit at the
new address — a non-trivial relocation. Confirm A vs B before building.

## Simplified design (assuming A — collract node in com)

- **com `.art`:** add a `TraceHub` node (distinct TIPC, e.g. 0x8001002D)
  with: `in_records` (TraceRecordSubmit receiver — FCs submit here),
  `stream_out` (TraceRecordStream sender — supdbg + others subscribe via
  TIPC), `ctl` (TraceControl). com's gRPC `TraceStream.Subscribe` hooks
  this node's stream directly (no TipcTraceUplink to a foreign daemon).
- **Trace enable** stays as-is: rf → com gRPC ConfigureTrace → supervisor
  → node. (Config still rides the supervisor so it survives restart.)
- **FC submit target** re-points from services/log's 0x80010013 to the
  com TraceHub address (gen-app already emits the submit wiring; just the
  netgraph address changes).
- **Local vs remote fanout:** the TraceHub fans `TraceRecordStream` to
  TIPC subscribers (supdbg) AND com's gRPC bridge forwards the same
  stream to remote rf — one collector, two egress paths.

## e2e steps (simplified, A)

1. rf → com `ConfigureTrace(node=sm, enabled=true)` → supervisor →
   NodeTraceCtl push to sm. Assert sm's Tracer turns on (a trace record
   appears).
2. sm emits → TIPC TraceRecordSubmit → com TraceHub.
3. rf → com `TraceStream.Subscribe` → receives sm's records over gRPC,
   decoded with google-protobuf. Assert the expected record (e.g. an
   sm state_transition) arrives.

## Open decisions (need user)

1. **A vs B** — relocate the collector into com (A, recommended) vs keep
   services/log + proxy (B, less churn). Drives everything below.
2. If A: **retire services/log's trace role, or keep services/log for
   plain logging** and only move the trace collector? (services/log also
   has a LogStream syslog role.)
3. TraceHub distinct TIPC address (proposal 0x8001002D — clear of FC
   range and the probe 0x800100FF / sm gate 0x8001001D).

# supervisor-gui: repair the dropped TraceStream gRPC client

**Status:** pre-existing breakage, surfaced 2026-05-27 by the `supervisor-gui/ →
tools/supervisor-gui/` move (the move made `bazel build
//tools/supervisor-gui:supervisor-gui` actually compile, where it had only been
`query`-checked before). The move itself is correct; this is unrelated drift.

## The break

`tools/supervisor-gui/src/grpc_client.cpp:52` calls
`::services::com::TraceStream::NewStub(...)`, but the `TraceStream` gRPC service
was **removed from `services/com/proto/supervisor_bridge.proto`** in commit
`992f80f` ("trace-egress: producer + reporting gate + collector's own gRPC; com
is proxy-only"). com is no longer in the trace byte path — trace EGRESS now
streams directly from the collector's own gRPC (`services/log/proto/
trace_stream.proto`, the `TraceStream.Subscribe` service there). So the GUI has
not compiled against the current proto since `992f80f` (well before the layout
housekeeping).

```
error: 'services::com::TraceStream' has not been declared
  tools/supervisor-gui/src/grpc_client.cpp:52
```

## The fix (when the GUI is picked back up)

The trace pane's client must move to the EGRESS-direct design (#399–#405):
- Point the trace stub at the collector's `trace_stream.proto`
  (`services/log/...`), not com's `supervisor_bridge.proto`. Connect to the
  collector endpoint (default `127.0.0.1:7710`), not the com bridge (`:7700`).
- The CONTROL half (arming a trace) stays on com:
  `SupervisorView.ConfigureTrace` / `GetTraceConfig` — those RPCs still exist.
- Decode `TraceRecord` via `libtrace_decoder` (#359/#360), same as supdbg's
  `trace stream --decode`.
- Or, if the GUI's trace pane is dead weight, `#if 0` / delete it — the GUI is
  external dev tooling (not a Theia platform node; see the `.art` removal this
  session) and rf-theia + supdbg are the supported observation paths.

## Caveat

Fixing the TraceStream symbol may uncover further proto/API drift behind it
(the GUI predates several proto reshapes). Budget a full GUI build-green pass,
not just a one-line swap. Lower priority — supdbg + rf-theia cover live
observation; the GUI is optional.

# FC-side config service — receive ConfigureLogLevel (and trace) live

Follow-up to #385. Closes the FC-side gap: a running daemon receives a
config push from the supervisor and applies it **without restart**,
over the **standard svc/comm header** (GwMessageHeader + the normal
`register_cast`/`register_call` path) — no bespoke control frame.

## Why the current push has no receiver

Investigation (this session) found two facts:

1. **No FC daemon binds inbound TIPC.** The gen-app `main.cc` only does
   `daemon.start()` (GenServer mailbox thread). It never stands up a
   `TipcMux` / `bind_node`, so a reporting node has **no listening
   socket** for the supervisor to reach. (`demo/src/p1_main.cc` shows
   the hand-written pattern that gen-app never templated.)
2. **The supervisor speaks a different wire format.**
   `send_frame_to_tipc_name()` writes a bespoke `[u16 tag BE][payload]`
   frame (kTagTraceApplyConfig=0x0300, kTagLogApplyConfig=0x0400). The
   runtime's `TipcMux` only decodes `GwMessageHeader`-framed app
   messages dispatched by `service_id` (djb2 hash of the message type).
   The two never meet — even with a listener bound, the frame wouldn't
   parse.

So both trace (#361) and log (#385) "push" into a void. The env path
(THEIA_LOG_LEVEL) saves the level for the *next* restart; nothing lands
live.

## Design — config service auto-injected by the `reporting` flag

Decisions (user):
- **Gate on `reporting`, not an explicit `config=`.** A `reporting`
  node already gets the heartbeat + trace-push wiring; it now also
  automatically gets the config-service receiver. No new per-node
  annotation — injection is automatic, same flag.
- **Keep the schema in the supervisor.** The gRPC/supdbg→supervisor
  `services.supervisor.LogLevelConfig{target_node, level}` (strings) in
  `platform/supervisor/system/package.art` is unchanged. For the
  supervisor→FC leg, a **scalar** nanopb mirror lives at
  `platform/proto/system/services/supervisor/supervisor_ctl.proto`,
  package `services_supervisor`, bundled into `platform_protos`.
- **Scalar enum, not string (nanopb constraint).** nanopb renders
  proto3 `string` as `pb_callback_t`; the runtime's `register_cast<Msg>`
  pb_decodes into a bare `Msg{}` with no callbacks, so string fields are
  dropped on receive. So the FC-facing push is `LogLevelPush { LogLevelValue
  level }` — an enum (LL_TRACE=0..LL_ERROR=4, ordinal-aligned with
  `platform::runtime::LogLevel`). `target_node` is dropped: the
  supervisor routes the cast to the node's TIPC address, so the daemon
  knows the frame is for itself.
- **This cut: LOG ONLY.** TraceConfig's `msg_type` is inherently a
  string, so trace needs the callback-string decode work first. Trace
  rides the same config-service receiver in a follow-up (#386 trace
  leg) once register_cast captures strings.

When `reporting`, gen-app injects a **config service**: inbound
`register_cast<services_supervisor_LogLevelPush>` bound on the node's
TIPC address, dispatching to `handle_cast(LogLevelPush&, State&)` →
`logger->set_level(LogLevel(level))`. The supervisor sends it as a
**standard `GW_MSG_GEN_CAST`** frame (GwMessageHeader,
`service_id = djb2("services_supervisor_LogLevelPush")`), exactly like
`RemoteRef::cast_`. The djb2 string matches the nanopb C type name on
both sides.

```
supdbg log set-level <node> <lvl>          (#385, already shipped)
  → services/com gRPC → supervisor (op_kind=11)
  → supervisor.push: cast(GwMessageHeader{CAST,
        service_id=djb2("services_..._LogLevelConfig")}, pb_bytes)
  → FC TipcMux (bound because node has `config=`)
      register_cast<...LogLevelConfig> decodes → enqueue
  → daemon.handle_cast(LogLevelConfig, State)
       logger_->set_level(parse_log_level(cfg.level))
```

No second wire format. The supervisor stops hand-rolling
`[tag][payload]` and uses the libgw frame the whole system already
speaks. `kTagTraceApplyConfig` / `kTagLogApplyConfig` are retired.

## Where the config message is declared

`LogLevelConfig` (and the trace config) must be a **nanopb-compiled**
message the FC links — i.e. live under
`platform/proto/system/services/<pkg>/`. Options to confirm in plan:

- a shared `services/system/<control>/package.art` whose package
  generates a nanopb proto every FC can import via `config = <FQN>`;
- or declare the config message in each FC's own package.art.

A shared control package is the cleaner match for "inject the same way"
— one `LogLevelConfig` referenced by FQN from any node's `config=`.

## Steps

1. **Control message in the runtime.** **DONE.** The message lives in
   the runtime, not the supervisor, because the runtime is the common
   dep of both sender (supervisor) and receiver (apps).
   `platform/runtime/system/package.art` (package `platform.runtime`)
   declares `enum LogLevelValue` + scalar `message LogLevelPush`.
   `gen-proto-package` → `platform/runtime/proto/platform_runtime/
   runtime.proto` (committed-as-source), nanopb-compiled by a genrule
   INTO `//platform/runtime:runtime`. C type
   `platform_runtime_LogLevelPush`.
2. **GenServer base handler + activation.** **DONE.** Handling lives
   in the runtime base class, the app only activates it (like the
   `reporting` conditional). `GenServer<Derived,State>` gains a
   non-virtual `handle_cast(const platform_runtime_LogLevelPush&,
   State&)` → `process_logger().set_level(LogLevel(level))`. Added
   `DEMO_DECLARE_REMOTE_CODEC(platform_runtime_LogLevelPush)` +
   `process_logger()`/`set_process_logger()` + `kDefaultLogLevel` in
   Logger.{hh,cc}. Runtime builds; all runtime tests pass.
3. **main.cc templates.** **DONE.** When `n.reporting`,
   `set_process_logger(logger)` at boot, then a process `TipcMux`
   binds each reporting node + `register_cast<platform_runtime_LogLevelPush>`
   + `start()/stop()`. Applied to both `main.cc.j2` and
   `main.statem.cc.j2`. Also added a `using
   GenServer<...>::handle_cast;` (and the GenStateM variant) to the
   daemon lib templates — a Derived handle_cast for a port message
   otherwise HIDES the base config handler by name (hit on `log`'s
   TraceCollector::handle_cast(TraceRecord)).
4. **Supervisor send-path switch.** **DONE.**
   `push_log_level_to_child` now sends a standard GW_MSG_GEN_CAST frame
   (`send_gw_cast_to_tipc_name`, packed 24-byte GwMessageHeader mirror +
   `service_id=djb2("platform_runtime_LogLevelPush")` + a hand-encoded
   `LogLevelPush` varint). No libgw link needed — the header is a packed
   struct, the payload one proto field. The bespoke 0x0400 tag is
   retired. Supervisor builds.
5. **Regen 5 FCs.** **DONE.** All 5 regen byte-stable
   (`fc_regen_stability` 5/5 PASS); all 5 FC mains + supervisor build;
   `gen_app_chain` 6/6 PASS (incl. Stage 6 bazel-build of a fresh
   reporting FC).
6. **Wire-compat verified.** **DONE.** A standalone check confirms the
   supervisor's `djb2` service_id (0x4446) == the FC's `register_cast`
   `RemoteCodec::service_id`, and the supervisor's hand-rolled payload
   nanopb-decodes to the right `LogLevelValue`. Full live TIPC smoke
   (running supervisor+FC+com) not required for correctness — both ends
   agree on service_id + wire bytes and every leg builds.

7. **LIVE end-to-end VERIFIED.** **DONE** (post per-node-logger work). Against
   the staged central stack (`install/central`, all 8 FCs forked):
   `ConfigureLogLevel(per_daemon, debug)` → supervisor `status:0 'log level
   applied'`, and **per_daemon's own file log** gained:
   `[INFO ] [#per_daemon] log level -> debug (supervisor push)` — i.e. the FC's
   `GenServer::handle_cast(LogLevelPush&)` fired and applied it. The whole chain
   (probe → supervisor → Registry resolve → GW_MSG_GEN_CAST → FC TipcMux
   register_cast → handle_cast → logger.set_level) confirmed live, no restart.

## Update — strengthened by the per-node logger work

The base handler now applies the level to the NODE's OWN tagged logger
(`this->log().set_level(lvl)`), not just `process_logger()` — so in a multi-node
process each node's level is set independently, and the "log level -> X
(supervisor push)" confirmation is written through that node's logger with its
`[#<node>]` tag. The supervisor send-path also moved off the hand-rolled GwHdrWire
onto the runtime RemoteRef / TheiaMsgHeader. Functionally complete + live-proven.

## Out of scope (this cut)

- **Trace (#361 FC leg).** `TraceConfig.msg_type` is a string; nanopb's
  pb_callback_t can't be captured by register_cast's bare decode, so
  trace needs the string-capture work (or a scalar reshape) first. It
  then rides the SAME GenServer config receiver. Deferred.
- supervisor-GUI "Set log level…" dialog (#385 deferred).
- A `call` (ack) variant — set-level is fire-and-forget `cast`.

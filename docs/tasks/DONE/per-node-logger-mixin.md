# Per-node logger — mixin on GenServer/GenRunnable, injected by main.cc

> **DONE (2026-06-04).** Landed across `dc02265` (runtime+supervisor per-node
> tagged loggers + selectable sinks), `12440c9` (supervisor engine logs via
> `lib/Log.hh`/process_logger), `1f1e654` (services FCs ported), `7186678`
> (demo ported), `8000c6b` (FileLogger mkdir -p). NodeLogger mixin +
> ContextLogger `[#<tag>]` + MakeLogger stdio|null|file|syslog + THEIA_LOGGER
> env all in `platform/runtime/include/`. The 4 raw `fprintf` left in supervisor
> `runtime.cpp` are post-fork/pre-execvp child errors (chdir/affinity/rlimit/
> execvp) — those MUST stay raw (async-signal-safety), not a leftover.

## Problem

Logging across an FC process is inconsistent and mis-tagged:

- The supervisor ENGINE (worker-side) logs via `process_logger()`, which
  main.cc published as the `[#supervisor_ctl]` ContextLogger — so worker-side
  lines wear the CTL node's tag.
- `GenServer` itself logs node events with raw
  `fprintf(stderr, "[%s] ...", Derived::kNodeName)` (GenServer.hh:291, 334) —
  it KNOWS the node identity but doesn't route through a tagged Logger.
- `SupervisorWorker_handlers.cc` + `main.cc` do raw `fprintf(stderr,"[%s]...",
  kNodeName)`.
- `process_logger()` is a SINGLE process-global sink + ONE tag — it can't
  attribute a line to the right node in a multi-node process.

`lib/Log.hh`'s `ContextLogger` already prepends `[#<tag>]` per node; the missing
piece is **giving each node its OWN tagged logger instance** instead of one
process-global tag.

## Design — a logger the node base classes hold

`main.cc` constructs each node's specialized (tagged) logger and injects it; the
base classes (`GenServerBase` / `GenRunnable`) hold it and expose `log()`. The
engine takes a `Logger&` from its owning node (the worker), so engine lines wear
`[#supervisor_worker]` and ctl-handler lines wear `[#supervisor_ctl]`.

### 1. Base holds a logger; node exposes log()

A shared mixin so both base classes get it identically. `NodeLogger` defaults to
the process logger so un-migrated nodes keep working.

```cpp
// platform/runtime/include/NodeLogger.hh  (new)
#pragma once
#include "Logger.hh"
#include <memory>
namespace theia::runtime {

// Mixed into GenServerBase + GenRunnable. Holds a per-node Logger; until one is
// set, falls back to the process logger so existing nodes are unaffected.
class NodeLogger {
public:
    void set_logger(std::shared_ptr<Logger> lg) noexcept {
        if (lg) logger_ = std::move(lg);
    }
    // The node's logger (tagged). Never null: process logger is the fallback.
    Logger& log() noexcept {
        return logger_ ? *logger_ : process_logger();
    }
private:
    std::shared_ptr<Logger> logger_;
};

}  // namespace theia::runtime
```

```cpp
// GenServer.hh — GenServerBase gains it:
class GenServerBase : public theia::runtime::NodeLogger { ... };

// GenRunnable.hh — GenRunnable gains it:
template <class Derived>
class GenRunnable : public theia::runtime::NodeLogger { ... };
```

Now ANY node (or the engine holding a node ref) calls `node.log().info("...")`.

### 2. GenServer's own raw fprintf → log()

Replace the two framework fprintf sites with the tagged logger (they already
have the right text; just route it):

```cpp
// handle_cast(LogLevelPush): GenServer.hh:291
- std::fprintf(stderr, "[%s] log level -> %s (supervisor push)\n",
-              Derived::kNodeName, log_level_name(lvl));
+ this->log().info(std::string("log level -> ") + log_level_name(lvl) +
+                  " (supervisor push)");
// handle_cast(TraceControlPush): GenServer.hh:334 — same treatment.
```

The `[#<tag>]` now comes from the ContextLogger, so drop the manual `[%s]`/
kNodeName prefix (the tag IS the node identity).

### 3. lib/Log.hh gains the per-node tag factory

`MakeContextLogger()` is hardcoded to one `kLogTag`. The generated `lib/Log.hh`
already knows each node's tag — give it a factory that stamps an explicit tag so
main.cc can build the ctl vs worker loggers:

```cpp
// lib/Log.hh (generated) — add:
inline std::shared_ptr<::theia::runtime::Logger>
MakeContextLogger(const char* tag) noexcept {
    return std::make_shared<ContextLogger>(tag,
        ::theia::runtime::MakeConsoleLogger());
}
// (ContextLogger ctor takes a tag instead of the file-scope kLogTag constant.)
```

For the supervisor there are two node tags — `supervisor_ctl` and
`supervisor_worker`. (Decide: one Log.hh with both tags, or the existing single
+ a worker variant. The generator emits Log.hh per FC; a multi-node FC should
expose each node's tag.)

### 4. main.cc injects per-node loggers

main.cc constructs each node, so it builds + assigns each node's tagged logger:

```cpp
// main.cc (generated)
SupervisorCtl    supervisor_ctl;
supervisor_ctl.set_logger(MakeContextLogger(SupervisorCtl::kNodeName));   // [#supervisor_ctl]
supervisor_ctl.start();

SupervisorWorker supervisor_worker;
supervisor_worker.set_logger(MakeContextLogger(SupervisorWorker::kNodeName)); // [#supervisor_worker]
supervisor_worker.start();
```

`set_process_logger()` can stay (LogLevelPush still flips it / it's the
fallback), but the PRIMARY path is the per-node logger.

### 5. Supervisor engine takes the worker's logger

The engine is owned by `SupervisorWorker`; pass the node's logger into the engine
so its lines wear `[#supervisor_worker]`:

```cpp
// SupervisorWorker::do_start()
g_engine = std::make_unique<Supervisor>(m.take_tree(), root);
g_engine->set_logger(&this->log());   // engine logs as supervisor_worker
```

Engine-side, replace the file-local `log_info/warn/err` helpers (and the 8 raw
fprintf in runtime.cpp) with `logger_->info/warn/error(...)` on the injected
`Logger&`. The helpers either vanish (inline the calls — user preference) or
become one-liners over `logger_`.

## Scope / sequencing

1. Add `NodeLogger.hh` + mix into both base classes (runtime). Default-fallback
   keeps every existing FC compiling untouched.
2. Route GenServer's 2 framework fprintf through `log()`.
3. lib/Log.hh: `MakeContextLogger(tag)` + ContextLogger(tag, sink) — and the
   gen-app generator emits it (so it survives regen).
4. main.cc (generated): `set_logger(MakeContextLogger(Node::kNodeName))` per
   node — generator change.
5. Supervisor engine: take `Logger&`, drop log_* helpers + raw fprintf.

Steps 1-2 are pure runtime (safe, additive). 3-4 touch the GENERATOR (artheia
gen-app) so the wiring survives regeneration — not a hand-edit of generated
main.cc/Log.hh. 5 is supervisor-local.

## Open questions

- One `lib/Log.hh` per FC currently emits ONE `kLogTag`. A multi-node FC
  (supervisor: ctl + worker) needs per-node tags — does the generator emit a
  tag per node, or does main.cc pass `Node::kNodeName` (a constexpr already on
  each node) straight into `MakeContextLogger(tag)`? The latter needs NO extra
  generated tag constants — `kNodeName` IS the tag source. Prefer that.
- Keep `process_logger()` at all? It's still the LogLevelPush target + the
  NodeLogger fallback. Keep as fallback; per-node logger is primary.
- `log_*` engine helpers: inline at call sites (user pref) vs keep as
  `logger_`-bound one-liners. Inlining is cleaner if the engine holds `logger_`.

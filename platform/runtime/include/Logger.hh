// theia::runtime::Logger
//
// Minimal logging surface. The app receives a shared_ptr<Logger> from
// the runtime so multiple components can share one sink without
// owning it. Implementations route to stderr (the default
// ConsoleLogger), to a file, or to journald — the app neither knows
// nor cares.
//
// Level filter (#383): the base Logger carries a coarse-grained
// LogLevel threshold (Info by default). The convenience shorthands
// (trace/debug/info/warn/error) short-circuit records below the
// threshold before invoking the virtual log() — so the filter is
// inherited by every subclass without subclasses having to opt in.
// Operators flip the threshold via the THEIA_LOG_LEVEL env var at
// process boot (gen-app's main.cc reads it) or via a future
// ConfigureLogLevel RPC at runtime.

#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace theia {
namespace runtime {

enum class LogLevel {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
};

// Single source of truth for the level a Logger starts at before the
// manifest (THEIA_LOG_LEVEL, sourced from the rig's per-process
// log_level) or a live ConfigureLogLevel push overrides it. Referenced
// by the Logger member initializer and parse_log_level's
// unknown-input fallback so the default lives in exactly one place.
inline constexpr LogLevel kDefaultLogLevel = LogLevel::Info;

// Parse "trace"|"debug"|"info"|"warn"|"error" (case-insensitive).
// Anything else returns Info — never throws, never aborts. Used by
// gen-app's main.cc to apply $THEIA_LOG_LEVEL at boot.
LogLevel parse_log_level(const std::string& name) noexcept;

// Inverse: render a LogLevel as the canonical lowercase name.
const char* log_level_name(LogLevel l) noexcept;

class Logger {
 public:
  virtual ~Logger() = default;

  // Plain-string sink. Implementations format the level + timestamp.
  // Called only from the level-filtered shorthand methods below.
  virtual void log(LogLevel level, const std::string& message) noexcept = 0;

  // Operator-controllable threshold. Atomic so a live
  // ConfigureLogLevel RPC can update from any thread without
  // tearing. Defaults to Info — Trace/Debug suppressed unless
  // explicitly enabled.
  void set_level(LogLevel l) noexcept {
    level_.store(l, std::memory_order_relaxed);
  }
  LogLevel level() const noexcept {
    return level_.load(std::memory_order_relaxed);
  }
  bool enabled(LogLevel l) const noexcept {
    return static_cast<int>(l) >= static_cast<int>(level());
  }

  // Convenience shorthands — non-virtual; short-circuit on the
  // level filter so callers don't pay for formatting a message
  // that would be dropped.
  void trace(const std::string& m) noexcept { if (enabled(LogLevel::Trace)) log(LogLevel::Trace, m); }
  void debug(const std::string& m) noexcept { if (enabled(LogLevel::Debug)) log(LogLevel::Debug, m); }
  void info (const std::string& m) noexcept { if (enabled(LogLevel::Info))  log(LogLevel::Info,  m); }
  void warn (const std::string& m) noexcept { if (enabled(LogLevel::Warn))  log(LogLevel::Warn,  m); }
  void error(const std::string& m) noexcept { if (enabled(LogLevel::Error)) log(LogLevel::Error, m); }

 private:
  std::atomic<LogLevel> level_{kDefaultLogLevel};
};

// Default implementation: writes "[LEVEL] message\n" to stderr.
class ConsoleLogger : public Logger {
 public:
  void log(LogLevel level, const std::string& message) noexcept override;
};

// Helper for the runtime to hand each app a sensible default.
std::shared_ptr<Logger> MakeConsoleLogger() noexcept;

// ---- Process-wide logger handle (#386) ----------------------------------
//
// A reporting FC daemon's config service receives a LogLevelPush from
// the supervisor and applies it with set_level — but the live logger is
// a local in main.cc, not a member of the daemon. Rather than thread a
// shared_ptr<Logger> through every generated daemon ctor, main.cc
// publishes the process logger here once at boot, and the generated
// handle_cast reaches it via process_logger(). Mirrors the
// tracer_for(kNodeName) process-wide accessor in Tracer.hh.
//
// set_process_logger is called exactly once, on the main thread, before
// any node starts. process_logger() is then safe to call from any node
// thread (set_level is atomic). Returns a never-null reference; if the
// app never published one, a lazily-created ConsoleLogger backs it so
// the handler can't crash.
void    set_process_logger(std::shared_ptr<Logger> logger) noexcept;
Logger& process_logger() noexcept;

}  // namespace runtime
}  // namespace theia

// platform::runtime::Logger
//
// Minimal logging surface. The app receives a shared_ptr<Logger> from
// the runtime so multiple components can share one sink without
// owning it. Implementations route to stderr (the default
// ConsoleLogger), to a file, or to journald — the app neither knows
// nor cares.

#pragma once

#include <memory>
#include <string>

namespace platform {
namespace runtime {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};

class Logger {
 public:
  virtual ~Logger() = default;

  // Plain-string sink. Implementations format the level + timestamp.
  virtual void log(LogLevel level, const std::string& message) noexcept = 0;

  // Convenience shorthands — non-virtual; forward to log().
  void trace(const std::string& m) noexcept { log(LogLevel::Trace, m); }
  void debug(const std::string& m) noexcept { log(LogLevel::Debug, m); }
  void info (const std::string& m) noexcept { log(LogLevel::Info,  m); }
  void warn (const std::string& m) noexcept { log(LogLevel::Warn,  m); }
  void error(const std::string& m) noexcept { log(LogLevel::Error, m); }
};

// Default implementation: writes "[LEVEL] message\n" to stderr.
class ConsoleLogger : public Logger {
 public:
  void log(LogLevel level, const std::string& message) noexcept override;
};

// Helper for the runtime to hand each app a sensible default.
std::shared_ptr<Logger> MakeConsoleLogger() noexcept;

}  // namespace runtime
}  // namespace platform

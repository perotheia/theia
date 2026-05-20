#include "Logger.hh"

#include <cstdio>

namespace platform {
namespace runtime {

namespace {
const char* level_tag(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?    ";
}
}  // namespace

void ConsoleLogger::log(LogLevel level, const std::string& message) noexcept {
    // Single fprintf so concurrent loggers don't interleave mid-line.
    std::fprintf(stderr, "[%s] %s\n", level_tag(level), message.c_str());
}

std::shared_ptr<Logger> MakeConsoleLogger() noexcept {
    return std::make_shared<ConsoleLogger>();
}

}  // namespace runtime
}  // namespace platform

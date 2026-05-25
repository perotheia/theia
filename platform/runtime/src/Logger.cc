#include "Logger.hh"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

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

std::string to_lower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(
        static_cast<unsigned char>(c))));
    return out;
}
}  // namespace

LogLevel parse_log_level(const std::string& name) noexcept {
    const std::string lo = to_lower(name);
    if (lo == "trace") return LogLevel::Trace;
    if (lo == "debug") return LogLevel::Debug;
    if (lo == "info")  return LogLevel::Info;
    if (lo == "warn" || lo == "warning") return LogLevel::Warn;
    if (lo == "error" || lo == "err")    return LogLevel::Error;
    // Unknown — fall back to the single default rather than failing.
    // Operator typos become quietly-lax, not noisy aborts.
    return kDefaultLogLevel;
}

const char* log_level_name(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
    }
    return "info";
}

void ConsoleLogger::log(LogLevel level, const std::string& message) noexcept {
    // Single fprintf so concurrent loggers don't interleave mid-line.
    std::fprintf(stderr, "[%s] %s\n", level_tag(level), message.c_str());
}

std::shared_ptr<Logger> MakeConsoleLogger() noexcept {
    return std::make_shared<ConsoleLogger>();
}

}  // namespace runtime
}  // namespace platform

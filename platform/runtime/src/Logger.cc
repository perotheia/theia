#include "Logger.hh"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

namespace theia {
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

// ---- Selectable sink kinds (THEIA_LOGGER) -------------------------------

namespace {

std::string iso8601_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// Recursive mkdir (mkdir -p). Best-effort: ignores EEXIST, gives up silently on
// any other error (the subsequent open() reports the real failure).
void mkdir_p(const std::string& dir) {
    if (dir.empty()) return;
    std::string acc;
    size_t i = 0;
    if (dir[0] == '/') { acc = "/"; i = 1; }
    while (i <= dir.size()) {
        if (i == dir.size() || dir[i] == '/') {
            if (!acc.empty() && acc != "/") {
                ::mkdir(acc.c_str(), 0755);  // EEXIST etc. ignored
            }
            if (i < dir.size()) acc += '/';
        } else {
            acc += dir[i];
        }
        ++i;
    }
}

// Drops everything.
class NullLogger : public Logger {
public:
    void log(LogLevel, const std::string&) noexcept override {}
};

// Appends "<ts> [LEVEL] message\n" to an fd. One write() per record so
// concurrent loggers can't interleave mid-line (PIPE/regular-file writes under
// the page size are atomic on Linux). Owns the fd; falls back to stderr if the
// open failed.
class FileLogger : public Logger {
public:
    explicit FileLogger(const std::string& path) {
        // Create the parent directory tree (mkdir -p) so file:/tmp/theia/x.log
        // works without the dir pre-existing — the default logger path the
        // manifest generator emits lives under /tmp/theia.
        if (auto slash = path.find_last_of('/'); slash != std::string::npos &&
            slash > 0) {
            mkdir_p(path.substr(0, slash));
        }
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                     0644);
        if (fd_ < 0) {
            std::fprintf(stderr,
                "[theia] logger: cannot open '%s' (%s) — using stderr\n",
                path.c_str(), std::strerror(errno));
            fd_ = STDERR_FILENO;
            owns_ = false;
        }
    }
    ~FileLogger() override { if (owns_ && fd_ >= 0) ::close(fd_); }

    void log(LogLevel level, const std::string& message) noexcept override {
        std::string line = iso8601_now();
        line += " [";
        line += level_tag(level);
        line += "] ";
        line += message;
        line += '\n';
        ssize_t n = ::write(fd_, line.data(), line.size());
        (void)n;  // best-effort; nothing useful to do on a failed log write
    }
private:
    int  fd_{-1};
    bool owns_{true};
};

// Routes to syslog(3). openlog ident must outlive the logger, so we own the
// string. LOG_USER facility; the per-record priority maps from LogLevel.
class SyslogLogger : public Logger {
public:
    explicit SyslogLogger(std::string ident) : ident_(std::move(ident)) {
        ::openlog(ident_.c_str(), LOG_PID | LOG_NDELAY, LOG_USER);
    }
    ~SyslogLogger() override { ::closelog(); }

    void log(LogLevel level, const std::string& message) noexcept override {
        int pri = LOG_INFO;
        switch (level) {
            case LogLevel::Trace: pri = LOG_DEBUG;   break;
            case LogLevel::Debug: pri = LOG_DEBUG;   break;
            case LogLevel::Info:  pri = LOG_INFO;    break;
            case LogLevel::Warn:  pri = LOG_WARNING; break;
            case LogLevel::Error: pri = LOG_ERR;     break;
        }
        ::syslog(pri, "%s", message.c_str());
    }
private:
    std::string ident_;
};

}  // namespace

std::shared_ptr<Logger> MakeNullLogger() noexcept {
    return std::make_shared<NullLogger>();
}
std::shared_ptr<Logger> MakeFileLogger(const std::string& path) noexcept {
    return std::make_shared<FileLogger>(path);
}
std::shared_ptr<Logger> MakeSyslogLogger(const std::string& ident) noexcept {
    return std::make_shared<SyslogLogger>(ident);
}

std::shared_ptr<Logger> MakeLogger(const std::string& spec,
                                   const std::string& ident) noexcept {
    // Split "<kind>[:<arg>]" on the FIRST ':' (a file path may contain more).
    std::string kind = spec, arg;
    if (auto pos = spec.find(':'); pos != std::string::npos) {
        kind = spec.substr(0, pos);
        arg  = spec.substr(pos + 1);
    }
    kind = to_lower(kind);
    if (kind.empty() || kind == "stdio") return MakeConsoleLogger();
    if (kind == "null")                  return MakeNullLogger();
    if (kind == "file")                  return MakeFileLogger(arg);
    if (kind == "syslog")                return MakeSyslogLogger(ident);
    // Unknown kind: lax fallback to stderr (don't fail process boot on a typo).
    std::fprintf(stderr,
        "[theia] logger: unknown kind '%s' in THEIA_LOGGER='%s' — using stdio\n",
        kind.c_str(), spec.c_str());
    return MakeConsoleLogger();
}

// ---- Process-wide logger handle (#386) ----------------------------------

namespace {
// Held by shared_ptr so the app's logger outlives any node thread that
// grabbed it. Published once on the main thread before nodes start, so
// no lock is needed for the publish/read race in practice; the
// shared_ptr copy in process_logger() is the only concurrent access and
// the pointer is set-once.
std::shared_ptr<Logger>& process_logger_slot() noexcept {
    static std::shared_ptr<Logger> slot;
    return slot;
}
}  // namespace

void set_process_logger(std::shared_ptr<Logger> logger) noexcept {
    process_logger_slot() = std::move(logger);
}

Logger& process_logger() noexcept {
    auto& slot = process_logger_slot();
    if (!slot) {
        // App never published one — back it with a console sink so a
        // config push (or any caller) can't dereference null.
        slot = MakeConsoleLogger();
    }
    return *slot;
}

}  // namespace runtime
}  // namespace theia

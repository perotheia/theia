// libtombstone implementation. See tombstone.h for design notes.

#include "tombstone/tombstone.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

namespace tombstone {

namespace {

// State captured by install_handlers, used inside the signal handler.
// All writes happen before any handler can fire; reads from the handler
// see a stable snapshot.
constexpr int kMaxBacktrace = 64;
constexpr int kMaxNameLen   = 64;
constexpr int kMaxDirLen    = 256;
char g_process_name[kMaxNameLen] = {0};
char g_output_dir [kMaxDirLen]   = {0};
// Alternate stack so we survive stack overflows. 64 KiB is generous.
constexpr size_t kAltStackSize = 64 * 1024;
unsigned char    g_alt_stack[kAltStackSize];

// Async-safe write of a const string.
void write_cstr(int fd, const char* s) {
    if (!s) return;
    size_t n = 0;
    while (s[n]) ++n;
    (void)write(fd, s, n);
}

// Async-safe write of a fixed-size buffer.
void write_buf(int fd, const char* p, size_t n) {
    (void)write(fd, p, n);
}

// Async-safe integer formatter (base 10, signed). Returns chars written.
size_t fmt_dec(char* buf, size_t cap, long long v) {
    if (cap < 2) return 0;
    char tmp[32];
    size_t i = 0;
    bool neg = false;
    if (v < 0) { neg = true; v = -v; }
    if (v == 0) tmp[i++] = '0';
    while (v > 0 && i < sizeof(tmp)) {
        tmp[i++] = '0' + static_cast<char>(v % 10);
        v /= 10;
    }
    size_t out = 0;
    if (neg && out < cap) buf[out++] = '-';
    while (i > 0 && out < cap) buf[out++] = tmp[--i];
    return out;
}

// Async-safe hex formatter (no 0x prefix; lowercase). Returns chars written.
size_t fmt_hex(char* buf, size_t cap, unsigned long long v) {
    static const char digits[] = "0123456789abcdef";
    if (cap == 0) return 0;
    char tmp[32];
    size_t i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v > 0 && i < sizeof(tmp)) {
        tmp[i++] = digits[v & 0xF];
        v >>= 4;
    }
    size_t out = 0;
    while (i > 0 && out < cap) buf[out++] = tmp[--i];
    return out;
}

void write_dec(int fd, long long v) {
    char b[32];
    write_buf(fd, b, fmt_dec(b, sizeof(b), v));
}

void write_hex(int fd, unsigned long long v) {
    write_cstr(fd, "0x");
    char b[32];
    write_buf(fd, b, fmt_hex(b, sizeof(b), v));
}

void write_iso_time(int fd) {
    // Async-safe-ish: time() is generally safe; localtime_r is allowed
    // by POSIX for signal handlers as long as no other thread is calling
    // localtime. Format as YYYY-MM-DDThh:mm:ss.
    time_t t = time(nullptr);
    struct tm tm_buf;
    if (!gmtime_r(&t, &tm_buf)) return;
    char b[24];
    size_t n = 0;
    n += fmt_dec(b + n, sizeof(b) - n, 1900 + tm_buf.tm_year);
    if (n < sizeof(b)) b[n++] = '-';
    if (tm_buf.tm_mon  + 1 < 10 && n < sizeof(b)) b[n++] = '0';
    n += fmt_dec(b + n, sizeof(b) - n, tm_buf.tm_mon + 1);
    if (n < sizeof(b)) b[n++] = '-';
    if (tm_buf.tm_mday < 10 && n < sizeof(b)) b[n++] = '0';
    n += fmt_dec(b + n, sizeof(b) - n, tm_buf.tm_mday);
    if (n < sizeof(b)) b[n++] = 'T';
    if (tm_buf.tm_hour < 10 && n < sizeof(b)) b[n++] = '0';
    n += fmt_dec(b + n, sizeof(b) - n, tm_buf.tm_hour);
    if (n < sizeof(b)) b[n++] = ':';
    if (tm_buf.tm_min  < 10 && n < sizeof(b)) b[n++] = '0';
    n += fmt_dec(b + n, sizeof(b) - n, tm_buf.tm_min);
    if (n < sizeof(b)) b[n++] = ':';
    if (tm_buf.tm_sec  < 10 && n < sizeof(b)) b[n++] = '0';
    n += fmt_dec(b + n, sizeof(b) - n, tm_buf.tm_sec);
    write_buf(fd, b, n);
}

const char* signame(int signo) {
    switch (signo) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
        case SIGSYS:  return "SIGSYS";
        case SIGTRAP: return "SIGTRAP";
        default:      return "?";
    }
}

const char* sigcode_name(int signo, int code) {
    if (signo == SIGSEGV) {
        switch (code) {
            case SEGV_MAPERR: return "SEGV_MAPERR";
            case SEGV_ACCERR: return "SEGV_ACCERR";
            default:          return "SEGV_?";
        }
    }
    if (signo == SIGBUS) {
        switch (code) {
            case BUS_ADRALN: return "BUS_ADRALN";
            case BUS_ADRERR: return "BUS_ADRERR";
            case BUS_OBJERR: return "BUS_OBJERR";
            default:         return "BUS_?";
        }
    }
    if (signo == SIGFPE) {
        switch (code) {
            case FPE_INTDIV: return "FPE_INTDIV";
            case FPE_INTOVF: return "FPE_INTOVF";
            case FPE_FLTDIV: return "FPE_FLTDIV";
            default:         return "FPE_?";
        }
    }
    return "?";
}

void copy_file_to_fd(int dst, const char* src_path) {
    int src = open(src_path, O_RDONLY | O_CLOEXEC);
    if (src < 0) return;
    char buf[4096];
    for (;;) {
        ssize_t n = read(src, buf, sizeof(buf));
        if (n <= 0) break;
        (void)write(dst, buf, static_cast<size_t>(n));
    }
    close(src);
}

void write_tombstone(int fd, int signo, siginfo_t* info, void* /*ucontext*/) {
    // Header section.
    write_cstr(fd, "*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***\n");
    write_cstr(fd, "tombstone for process \"");
    write_cstr(fd, g_process_name[0] ? g_process_name : "?");
    write_cstr(fd, "\" — ");
    write_iso_time(fd);
    write_cstr(fd, "Z\n");
    write_cstr(fd, "pid: ");        write_dec(fd, getpid());
    write_cstr(fd, ", tid: ");      write_dec(fd, syscall(186 /*SYS_gettid*/));
    write_cstr(fd, "\n");
    write_cstr(fd, "signal: ");     write_dec(fd, signo);
    write_cstr(fd, " (");           write_cstr(fd, signame(signo));
    write_cstr(fd, "), code: ");    write_dec(fd, info ? info->si_code : 0);
    write_cstr(fd, " (");           write_cstr(fd, sigcode_name(signo, info ? info->si_code : 0));
    write_cstr(fd, ")\n");
    if (info && (signo == SIGSEGV || signo == SIGBUS || signo == SIGILL ||
                 signo == SIGFPE  || signo == SIGTRAP)) {
        write_cstr(fd, "fault addr: ");
        write_hex(fd, reinterpret_cast<unsigned long long>(info->si_addr));
        write_cstr(fd, "\n");
    }

    // Backtrace. backtrace() / backtrace_symbols_fd() are not formally
    // async-signal-safe in glibc but are widely used in crash handlers
    // — best-effort matches debuggerd's spirit.
    write_cstr(fd, "\n--- backtrace ---\n");
    void* frames[kMaxBacktrace];
    int nframes = backtrace(frames, kMaxBacktrace);
    if (nframes > 0) {
        backtrace_symbols_fd(frames, nframes, fd);
    } else {
        write_cstr(fd, "(no frames captured)\n");
    }

    // Memory map.
    write_cstr(fd, "\n--- /proc/self/maps ---\n");
    copy_file_to_fd(fd, "/proc/self/maps");

    // Status snapshot.
    write_cstr(fd, "\n--- /proc/self/status ---\n");
    copy_file_to_fd(fd, "/proc/self/status");

    write_cstr(fd, "\n--- end ---\n");
}

void signal_handler(int signo, siginfo_t* info, void* ucontext) {
    // Build the tombstone path: <dir>/tombstone-<name>-<pid>-<ts>.txt
    // All buffer-based — no allocation.
    char path[512];
    size_t p = 0;
    auto cat = [&](const char* s) {
        while (*s && p + 1 < sizeof(path)) path[p++] = *s++;
    };
    auto cat_dec = [&](long long v) {
        char b[32];
        size_t n = fmt_dec(b, sizeof(b), v);
        for (size_t i = 0; i < n && p + 1 < sizeof(path); ++i) path[p++] = b[i];
    };
    cat(g_output_dir[0] ? g_output_dir : "/tmp");
    cat("/tombstone-");
    cat(g_process_name[0] ? g_process_name : "process");
    cat("-");
    cat_dec(getpid());
    cat("-");
    cat_dec(time(nullptr));
    cat(".txt");
    path[p] = '\0';

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        // Fall back to stderr so at least *something* shows up.
        fd = STDERR_FILENO;
    }

    write_tombstone(fd, signo, info, ucontext);

    if (fd != STDERR_FILENO) close(fd);

    // Re-raise the signal with the default handler so the process dies
    // as it would have, and the kernel reports the correct exit status
    // (WIFSIGNALED, WTERMSIG=signo) to our parent supervisor.
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigaction(signo, &dfl, nullptr);
    raise(signo);
}

}  // namespace

bool install_handlers(const char* process_name, const char* output_dir) {
    // Stash names into our static buffers.
    if (process_name) {
        size_t n = 0;
        while (process_name[n] && n + 1 < kMaxNameLen) {
            g_process_name[n] = process_name[n];
            ++n;
        }
        g_process_name[n] = '\0';
    }
    if (output_dir) {
        size_t n = 0;
        while (output_dir[n] && n + 1 < kMaxDirLen) {
            g_output_dir[n] = output_dir[n];
            ++n;
        }
        g_output_dir[n] = '\0';
        mkdir(g_output_dir, 0755);  // best-effort; existing is fine
    }

    // Set up the alternate stack so SIGSEGV-on-stack-overflow still fires
    // the handler. Has to be allocated before sigaction.
    stack_t alt;
    memset(&alt, 0, sizeof(alt));
    alt.ss_sp    = g_alt_stack;
    alt.ss_size  = sizeof(g_alt_stack);
    alt.ss_flags = 0;
    if (sigaltstack(&alt, nullptr) < 0) return false;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigfillset(&sa.sa_mask);
    sa.sa_sigaction = signal_handler;
    sa.sa_flags     = SA_RESTART | SA_SIGINFO | SA_ONSTACK;

    static const int fatal[] = { SIGSEGV, SIGABRT, SIGBUS, SIGFPE,
                                 SIGILL,  SIGSYS,  SIGTRAP };
    for (int s : fatal) {
        if (sigaction(s, &sa, nullptr) < 0) return false;
    }
    return true;
}

}  // namespace tombstone

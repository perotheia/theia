# Tombstones: crash forensics

When a daemon dies from a fatal signal, we want enough information at
the scene to know what happened. Restart alone is not debugging — a
process that crashes once a week needs the crash captured, not just
restarted.

`libtombstone` writes a text file per crash with signal info, a
backtrace, and the process's memory map at the moment of death. The
supervisor surfaces the tombstone path in its log when it observes the
child's signal-induced exit. Together they give you a crash record
without coredumps, gdb scripts, or post-mortem ssh sessions.

## Inspiration

Android's `debuggerd` does the same job for app processes. References:

- AOSP `system/core/debuggerd/handler/debuggerd_handler.cpp` —
  `sigaction(SA_SIGINFO | SA_ONSTACK)` for fatal signals.
- AOSP `system/core/debuggerd/libdebuggerd/tombstone.cpp` — the
  tombstone format.

`debuggerd` forks a separate `crash_dump` process and ptrace-attaches
the failing process so the dump can do heavy work (symbol resolution,
unwinding) outside the crashed address space. We don't. Our daemons
are small and the in-process handler is enough; if it ever isn't,
the fork-to-crash_dump model is a known upgrade path.

## What's collected

A tombstone is a UTF-8 text file with the layout below. Section
markers (`*** *** *** ***` for the top, `--- name ---` for each
section) follow Android's spirit, not its exact bytes — we use field
names that mean something in our system.

```
*** *** *** *** *** *** *** *** *** *** *** *** *** *** *** ***
tombstone for process "crasher" — 2026-05-20T21:37:03Z
pid: 2408291, tid: 2408291
signal: 11 (SIGSEGV), code: 1 (SEGV_MAPERR)
fault addr: 0x0

--- backtrace ---
/home/axadmin/repo/theia_runtime/platform/supervisor/build/crasher(+0x2558)[0x5ef35c13d558]
/lib/x86_64-linux-gnu/libc.so.6(+0x42520)[0x79fcd0842520]
./platform/supervisor/build/crasher(+0x145f)[0x61453f7c745f]
/lib/x86_64-linux-gnu/libc.so.6(+0x29d90)[0x79fcd0829d90]
/lib/x86_64-linux-gnu/libc.so.6(__libc_start_main+0x80)[0x79fcd0829e40]
./platform/supervisor/build/crasher(+0x15c5)[0x61453f7c75c5]

--- /proc/self/maps ---
61453f7c6000-61453f7c7000 r--p 00000000 103:02 6171810      /…/crasher
61453f7c7000-61453f7c9000 r-xp 00001000 103:02 6171810      /…/crasher
61453f7c9000-61453f7ca000 r--p 00003000 103:02 6171810      /…/crasher
...
79fcd0800000-79fcd0828000 r--p 00000000 103:02 13109399     /usr/lib/x86_64-linux-gnu/libc.so.6
...

--- /proc/self/status ---
Name:	crasher
Umask:	0022
State:	R (running)
Tgid:	2408291
Pid:	2408291
...

--- end ---
```

Sections, in order:

| Section | Contents |
|---|---|
| header | process name, ISO-8601 UTC timestamp, pid/tid |
| signal block | `signal: N (NAME)`, `code: N (NAME)`, optional `fault addr` for memory-related signals |
| `--- backtrace ---` | symbolic-ish backtrace from `backtrace()` + `backtrace_symbols_fd()`. Addresses + module names; no source lines (no DWARF lookup in-process). |
| `--- /proc/self/maps ---` | full memory map. Combine with the backtrace addresses to resolve modules by hand: find the address's mapping, subtract the mapping's base, and look up the offset in the binary with `addr2line` or `objdump`. |
| `--- /proc/self/status ---` | thread/memory state at death. Threads count, RSS, signal mask, capabilities. |

Notes on the backtrace:
- Symbols come from the dynamic symbol table — exported names are
  resolved (`__libc_start_main`), static-local frames show as
  `binary(+0xOFFSET)`. Strip stripped binaries lose symbols entirely.
- We do not run `addr2line` at dump time. That's a deliberate choice
  — running anything fancy from a signal handler is a recipe for the
  tombstone failing silently. Resolve offline with the supervisor's
  log line as your starting point.

## Signals handled

```
SIGSEGV  SIGABRT  SIGBUS  SIGFPE  SIGILL  SIGSYS  SIGTRAP
```

Anything else (SIGTERM, SIGKILL, SIGINT, normal exit) is left alone.
SIGKILL can't be caught at all; SIGTERM is the supervisor's graceful
stop and shouldn't dump.

## Integration

### Linking

```cmake
add_executable(my_daemon ...)
target_link_libraries(my_daemon PRIVATE tombstone)
```

The library is a static archive (`libtombstone.a`); it pulls in
nothing fancy on Linux (libgcc for `backtrace`).

### Setup at startup

```cpp
#include "tombstone/tombstone.h"

int main() {
    if (!tombstone::install_handlers("my_daemon", "/var/log/tombstones")) {
        // Non-fatal: log it but continue. The daemon still runs, you
        // just won't get tombstones if it crashes.
        std::fprintf(stderr, "tombstone setup failed\n");
    }
    // ... daemon main loop ...
}
```

`install_handlers` is idempotent in the sense that you can call it
again from the supervisor's restart cycle — the alternate stack and
signal handlers are reinstalled cleanly.

### Output path

`<output_dir>/tombstone-<process_name>-<pid>-<unix_ts>.txt`

The directory is created if missing (`mkdir(0755)`). The supervisor
expects this exact filename pattern when it looks up tombstones —
keep the prefix.

## Async-signal-safe constraints

The handler runs from a signal handler. POSIX defines a list of
async-signal-safe functions (`signal(7)` / `signal-safety(7)`).
`printf`, `malloc`, `std::string`, any libstdc++ stream, locale code —
all unsafe. Our handler:

- formats numbers into stack buffers with hand-rolled `fmt_dec` /
  `fmt_hex` instead of `snprintf`;
- writes via `write(2)` instead of `fprintf`;
- copies `/proc/self/maps` and `/proc/self/status` with raw
  `read/write` loops;
- uses a 64 KiB `sigaltstack` so a SIGSEGV from a recursive stack
  overflow still has a stack to run on (SA_ONSTACK).

`backtrace()` and `backtrace_symbols_fd()` are *not* officially
async-signal-safe in glibc — they can call `dlopen` to load
`libgcc_s.so.1` on first use. `debuggerd` accepts the same caveat for
the same reason: nothing else gets you a backtrace from a signal
handler without significantly more code. We mitigate by linking
`libgcc_s` at load time (it's already in the runtime image), so the
first crash doesn't trigger a `dlopen` on the failing path.

What this means in practice: a tombstone may fail to write if the
process is sufficiently corrupted (allocator state trashed in a
detectable way, the binary itself unmapped, FS full). That's
acceptable — partial information beats no information.

## Supervisor surfacing

When the supervisor observes a `WIFSIGNALED` exit, it walks the
supervisor tree up to the root looking for a `tombstone_dir`
configured on any ancestor. If found, it lists the directory for files
matching `tombstone-<child_name>-<old_pid>-*` and logs the newest as:

```
ERROR supervisor: tombstone for crasher (pid=2408291): /tmp/tombstones/tombstone-crasher-2408291-1779313023.txt
```

This way the log line you see *next to* the crash report tells you
exactly which file to open. The supervisor never reads the tombstone
content — it surfaces the path and moves on with restart strategy.

### Manifest configuration

```yaml
# executor.yaml — root supervisor sets the directory.
name: root
strategy: rest_for_one
tombstone_dir: /var/log/tombstones      # ← here
max_restarts: 3
max_seconds: 5
children:
  - name: my_daemon
    start_cmd: [/usr/bin/my_daemon]
    restart: permanent
    ...
```

The directory must be writable by the child processes. The supervisor
itself doesn't write tombstones — only daemons that link
`libtombstone` do.

## End-to-end demo

```sh
# Build everything.
cmake -S platform/supervisor -B platform/supervisor/build
cmake --build platform/supervisor/build

# Run a daemon that's going to crash on purpose.
cat > /tmp/crash_demo.yaml <<'YAML'
name: root
strategy: one_for_one
max_restarts: 5
max_seconds: 30
tombstone_dir: /tmp/tombstones
children:
- name: crasher
  start_cmd: [platform/supervisor/build/crasher, --mode, segv, --delay, "2"]
  restart: permanent
  shutdown: 5000
  type: worker
YAML

./platform/supervisor/build/supervisor run /tmp/crash_demo.yaml
```

What you see in the log:

```
INFO  supervisor: starting child crasher: …/crasher --mode segv --delay 2
INFO  supervisor: child crasher exited (code=-11, abnormal) — sup=root strategy=one_for_one
ERROR supervisor: tombstone for crasher (pid=…): /tmp/tombstones/tombstone-crasher-…-….txt
INFO  supervisor: starting child crasher: …/crasher --mode segv --delay 2
...
ERROR supervisor: supervisor root exceeded restart intensity (5 in 30s) — escalating
```

## What's not in scope

- **Symbol resolution.** Backtraces give addresses + module names.
  Resolve offline with `addr2line -e <binary> <offset>` or the AOSP
  `stack` tool ported to point at our binaries.
- **Register dump.** AOSP includes the CPU register file at the
  faulting instruction. We don't yet — adding it means
  CPU-architecture-specific code (`<ucontext.h>` on Linux, different
  layouts for x86_64 / aarch64). Worth doing once we land on aarch64
  targets.
- **Stack memory dump.** A small window around `$rsp` / `$sp`. Nice
  for understanding what's on the stack but doesn't help if you can't
  read the symbol table anyway.
- **Thread list.** AOSP dumps every thread's backtrace. We dump the
  faulting thread only. Multi-threaded daemons will lose this
  context; revisit when we have one.
- **Bash daemons.** A `bash` daemon (current fake-daemons) doesn't
  link `libtombstone` and won't write a tombstone if it dies. The
  supervisor will simply not find one and won't log the surfacing
  line. If you want crash traces for shell wrappers, configure the
  kernel `core_pattern` instead.

## Files

- `platform/supervisor/tombstone/include/tombstone/tombstone.h` — public API.
- `platform/supervisor/tombstone/src/tombstone.cpp` — implementation.
- `platform/supervisor/tombstone/demo/crasher.cpp` — demo daemon
  (`--mode {segv,abort,div0,none}`).
- `platform/supervisor/src/runtime.cpp` — the supervisor side:
  `find_tombstone_dir`, `locate_tombstone`, the `tombstone for …`
  log line.

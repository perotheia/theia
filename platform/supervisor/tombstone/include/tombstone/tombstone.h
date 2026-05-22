// libtombstone — in-process crash forensics.
//
// Inspired by Android's debuggerd. The caller links this library, calls
// tombstone::install_handlers(name, dir) once at startup, and from then
// on a fatal signal (SIGSEGV / SIGABRT / SIGBUS / SIGFPE / SIGILL /
// SIGSYS) writes a tombstone file to <dir>/tombstone-<name>-<pid>-<ts>.txt
// before re-raising the signal so the process dies as it would have.
//
// The format is vaguely Android-shaped (section markers, fault details,
// backtrace, /proc/self/maps) but uses our own field names. See
// docs/supervision.md for what the supervisor expects to find.
//
// Design constraints:
//
//  - Async-signal-safe handler. No printf, no malloc, no std::string in
//    the hot path. Format into stack buffers and write(2) directly.
//  - SA_ONSTACK + sigaltstack so a stack overflow doesn't take us out.
//  - Best-effort: a thoroughly corrupted process may fail mid-write;
//    that's better than no tombstone at all.

#pragma once

namespace tombstone {

// Install fatal-signal handlers. `process_name` is the short name we
// embed in the tombstone filename. `output_dir` is created if missing.
// Returns true on success.
bool install_handlers(const char* process_name, const char* output_dir);

}  // namespace tombstone

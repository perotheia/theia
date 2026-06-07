// User handler bodies for LogDaemon.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/LogDaemon.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/LogDaemon.hh"

#include <cstdio>

namespace ara::log {


// ---- OTP init/1 — runs once on the node thread after start(), before
//      the first message. Bootstrap a self-driving node's work loop here
//      (e.g. ::theia::runtime::post_info(*this, "tick")); leave empty for a
//      passive node. State fields live in impl/LogDaemon_state.hh.
void LogDaemon::init(LogDaemonState& /*s*/) {
}

// ---- string handle_info — the post_info()/send_after() tick path.
void LogDaemon::handle_info(const char* /*info*/, LogDaemonState& /*s*/) {
}

// (No log-ingest handler: logging is direct-to-sink, not a TIPC LogStream.
// LogDaemon has no ports — the old handle_cast(LogRecord) was removed with the
// phantom LogStream interface.)


}  // namespace ara::log

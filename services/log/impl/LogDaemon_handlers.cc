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



void LogDaemon::handle_cast(const LogRecord& /*msg*/,
                                 LogDaemonState& /*s*/) {
    // TODO: react to LogRecord (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received LogRecord\n",
                 kNodeName);
}




}  // namespace ara::log

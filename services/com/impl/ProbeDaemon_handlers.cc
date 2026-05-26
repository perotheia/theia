// User handler bodies for ProbeDaemon.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/ProbeDaemon.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/ProbeDaemon.hh"

#include <cstdio>

namespace ara::com {





ComEmpty ProbeDaemon::handle_call(
        const ComEmpty& /*req*/,
        ProbeDaemonState& /*s*/) {
    // TODO: implement Ping (ComEmpty →
    //                                 ComEmpty).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] Ping called\n",
                 kNodeName);
    return ComEmpty{};
}


}  // namespace ara::com

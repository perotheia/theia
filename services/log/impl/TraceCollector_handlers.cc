// User handler bodies for TraceCollector.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/TraceCollector.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/TraceCollector.hh"

#include <cstdio>

namespace ara::log {



void TraceCollector::handle_cast(const TraceRecord& /*msg*/,
                                 TraceCollectorState& /*s*/) {
    // TODO: react to TraceRecord (dispatched by message type; one
    // or more receiver ports may deliver it).
    std::fprintf(stderr, "[%s] received TraceRecord\n",
                 kNodeName);
}



TraceEmpty TraceCollector::handle_call(
        const TraceConfigRequest& /*req*/,
        TraceCollectorState& /*s*/) {
    // TODO: implement Configure (TraceConfigRequest →
    //                                 TraceEmpty).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] Configure called\n",
                 kNodeName);
    return TraceEmpty{};
}



}  // namespace ara::log

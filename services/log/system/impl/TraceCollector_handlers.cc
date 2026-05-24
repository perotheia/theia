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
    // TODO: react to in_records.rec (TraceRecordSubmit).
    std::fprintf(stderr, "[%s] received TraceRecordSubmit::rec\n",
                 kNodeName);
}

TraceEmpty TraceCollector::handle_call(
        const TraceConfigRequest& /*req*/,
        TraceCollectorState& /*s*/) {
    // TODO: implement TraceControl::Configure.
    std::fprintf(stderr, "[%s] TraceControl::Configure called\n",
                 kNodeName);
    return TraceEmpty{};
}

TraceEmpty TraceCollector::handle_call(
        const TraceConfigRequest& /*req*/,
        TraceCollectorState& /*s*/) {
    // TODO: implement TraceControl::Configure.
    std::fprintf(stderr, "[%s] TraceControl::Configure called\n",
                 kNodeName);
    return TraceEmpty{};
}


}  // namespace ara::log

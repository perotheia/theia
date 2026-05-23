// User handler bodies for PerDaemon.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/PerDaemon.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/PerDaemon.hh"

#include <cstdio>

namespace system_services_per {



PerValue PerDaemon::handle_call(
        const PerKey& /*req*/,
        PerDaemonState& /*s*/) {
    // TODO: implement PersistencyIf::Get.
    std::fprintf(stderr, "[%s] PersistencyIf::Get called\n",
                 kNodeName);
    return PerValue{};
}

PerEmpty PerDaemon::handle_call(
        const PerPut& /*req*/,
        PerDaemonState& /*s*/) {
    // TODO: implement PersistencyIf::Put.
    std::fprintf(stderr, "[%s] PersistencyIf::Put called\n",
                 kNodeName);
    return PerEmpty{};
}


}  // namespace system_services_per

// User handler bodies for ComDaemon.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/ComDaemon.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/ComDaemon.hh"

#include <cstdio>

namespace system_services_com {



ComEmpty ComDaemon::handle_call(
        const ComEmpty& /*req*/,
        ComDaemonState& /*s*/) {
    // TODO: implement ComBridge::Ping.
    std::fprintf(stderr, "[%s] ComBridge::Ping called\n",
                 kNodeName);
    return ComEmpty{};
}

ComEmpty ComDaemon::handle_call(
        const NetworkBindingRequest& /*req*/,
        ComDaemonState& /*s*/) {
    // TODO: implement ComCtl::EnableBindings.
    std::fprintf(stderr, "[%s] ComCtl::EnableBindings called\n",
                 kNodeName);
    return ComEmpty{};
}


}  // namespace system_services_com

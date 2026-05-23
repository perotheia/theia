// User handler bodies for UcmDaemon.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/UcmDaemon.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/UcmDaemon.hh"

#include <cstdio>

namespace system_services_ucm {



UcmReply UcmDaemon::handle_call(
        const UcmRequest& /*req*/,
        UcmDaemonState& /*s*/) {
    // TODO: implement UpdateCtl::RequestUpdate.
    std::fprintf(stderr, "[%s] UpdateCtl::RequestUpdate called\n",
                 kNodeName);
    return UcmReply{};
}


}  // namespace system_services_ucm

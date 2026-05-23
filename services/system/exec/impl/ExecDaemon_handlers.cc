// User handler bodies for ExecDaemon.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/ExecDaemon.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/ExecDaemon.hh"

#include <cstdio>

namespace ara::exec {



ExecEmpty ExecDaemon::handle_call(
        const StartGroupRequest& /*req*/,
        ExecDaemonState& /*s*/) {
    // TODO: implement ExecCtl::StartGroup.
    std::fprintf(stderr, "[%s] ExecCtl::StartGroup called\n",
                 kNodeName);
    return ExecEmpty{};
}

ExecEmpty ExecDaemon::handle_call(
        const StopGroupRequest& /*req*/,
        ExecDaemonState& /*s*/) {
    // TODO: implement ExecCtl::StopGroup.
    std::fprintf(stderr, "[%s] ExecCtl::StopGroup called\n",
                 kNodeName);
    return ExecEmpty{};
}


}  // namespace ara::exec

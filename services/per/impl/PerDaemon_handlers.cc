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

namespace ara::per {


// ---- OTP init/1 — runs once on the node thread after start(), before
//      the first message. Bootstrap a self-driving node's work loop here
//      (e.g. ::theia::runtime::post_info(*this, "tick")); leave empty for a
//      passive node. State fields live in impl/PerDaemon_state.hh.
void PerDaemon::init(PerDaemonState& /*s*/) {
}

// ---- string handle_info — the post_info()/send_after() tick path.
void PerDaemon::handle_info(const char* /*info*/, PerDaemonState& /*s*/) {
}




PerValue PerDaemon::handle_call(
        const PerKey& /*req*/,
        PerDaemonState& /*s*/) {
    // TODO: implement Get (PerKey →
    //                                 PerValue).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] Get called\n",
                 kNodeName);
    return PerValue{};
}

PerEmpty PerDaemon::handle_call(
        const PerPut& /*req*/,
        PerDaemonState& /*s*/) {
    // TODO: implement Put (PerPut →
    //                                 PerEmpty).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] Put called\n",
                 kNodeName);
    return PerEmpty{};
}


}  // namespace ara::per

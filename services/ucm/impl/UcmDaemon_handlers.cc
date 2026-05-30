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

namespace ara::ucm {


// ---- OTP init/1 — runs once on the node thread after start(), before
//      the first message. Bootstrap a self-driving node's work loop here
//      (e.g. ::theia::runtime::post_info(*this, "tick")); leave empty for a
//      passive node. State fields live in impl/UcmDaemon_state.hh.
void UcmDaemon::init(UcmDaemonState& /*s*/) {
}

// ---- string handle_info — the post_info()/send_after() tick path.
void UcmDaemon::handle_info(const char* /*info*/, UcmDaemonState& /*s*/) {
}




UcmReply UcmDaemon::handle_call(
        const UcmRequest& /*req*/,
        UcmDaemonState& /*s*/) {
    // TODO: implement RequestUpdate (UcmRequest →
    //                                 UcmReply).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] RequestUpdate called\n",
                 kNodeName);
    return UcmReply{};
}


}  // namespace ara::ucm

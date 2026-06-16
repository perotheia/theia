// User handler bodies for RdsCtl.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/RdsCtl.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/RdsCtl.hh"

#include <cstdio>

namespace ara::rds {


// ---- OTP init/1 — runs once on the node thread after start(), before
//      the first message. Bootstrap a self-driving node's work loop here
//      (e.g. ::theia::runtime::post_info(*this, "tick")); leave empty for a
//      passive node. State fields live in impl/RdsCtl_state.hh.
void RdsCtl::init(RdsCtlState& /*s*/) {
}

// ---- string handle_info — the post_info()/send_after() tick path.
void RdsCtl::handle_info(const char* /*info*/, RdsCtlState& /*s*/) {
}




RdsStatus RdsCtl::handle_call(
        const GetRdsStatus& /*req*/,
        RdsCtlState& /*s*/) {
    // TODO: implement GetStatus (GetRdsStatus →
    //                                 RdsStatus).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] GetStatus called\n",
                 kNodeName);
    // Stub status: report the broker as up with no registered streams. A
    // future impl can probe RouDi / count registered ports.
    RdsStatus reply{};
    reply.roudi_up  = true;
    reply.n_streams = 0;
    return reply;
}


}  // namespace ara::rds

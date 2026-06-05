// User handler bodies for PerClient.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/PerClient.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/PerClient.hh"

#include <cstdio>

namespace system_services_per {


// ---- OTP init/1 — runs once on the node thread after start(), before
//      the first message. Bootstrap a self-driving node's work loop here
//      (e.g. ::theia::runtime::post_info(*this, "tick")); leave empty for a
//      passive node. State fields live in impl/PerClient_state.hh.
void PerClient::init(PerClientState& /*s*/) {
}

// ---- string handle_info — the post_info()/send_after() tick path.
void PerClient::handle_info(const char* /*info*/, PerClientState& /*s*/) {
}




ConfigSnapshot PerClient::handle_call(
        const GetConfigReq& /*req*/,
        PerClientState& /*s*/) {
    // TODO: implement GetConfig (GetConfigReq →
    //                                 ConfigSnapshot).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] GetConfig called\n",
                 kNodeName);
    return ConfigSnapshot{};
}

PerReply PerClient::handle_call(
        const PutConfigReq& /*req*/,
        PerClientState& /*s*/) {
    // TODO: implement PutConfig (PutConfigReq →
    //                                 PerReply).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] PutConfig called\n",
                 kNodeName);
    return PerReply{};
}

PerReply PerClient::handle_call(
        const WatchConfigReq& /*req*/,
        PerClientState& /*s*/) {
    // TODO: implement WatchConfig (WatchConfigReq →
    //                                 PerReply).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] WatchConfig called\n",
                 kNodeName);
    return PerReply{};
}


}  // namespace system_services_per

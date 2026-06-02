// User handler bodies for ShwaDaemon.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/ShwaDaemon.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/ShwaDaemon.hh"

#include <cstdio>

namespace ara::shwa {


// ---- OTP init/1 — runs once on the node thread after start(), before
//      the first message. Bootstrap a self-driving node's work loop here
//      (e.g. ::theia::runtime::post_info(*this, "tick")); leave empty for a
//      passive node. State fields live in impl/ShwaDaemon_state.hh.
void ShwaDaemon::init(ShwaDaemonState& /*s*/) {
}

// ---- string handle_info — the post_info()/send_after() tick path.
void ShwaDaemon::handle_info(const char* /*info*/, ShwaDaemonState& /*s*/) {
}





}  // namespace ara::shwa

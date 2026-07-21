// User handler bodies for FrameProducer.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/FrameProducer.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/FrameProducer.hh"

#include <cstdio>

namespace ara::rdsdemo {


// ---- OTP init/1 — runs once on the node thread after start(), before
//      the first message. Bootstrap a self-driving node's work loop here
//      (e.g. ::theia::runtime::post_info(*this, "tick")); leave empty for a
//      passive node. State fields live in impl/FrameProducer_state.hh.
void FrameProducer::init(FrameProducerState& /*s*/) {
}

// ---- string handle_info — the post_info()/send_after() tick path.
void FrameProducer::handle_info(const char* /*info*/, FrameProducerState& /*s*/) {
}





}  // namespace ara::rdsdemo

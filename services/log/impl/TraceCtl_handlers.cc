// User handler bodies for TraceCtl.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/TraceCtl.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/TraceCtl.hh"

#include <cstdio>

#include "impl/trace_hub.hpp"   // process-global TraceHub (shared with the pump)

namespace ara::log {


// ---- OTP init/1 — runs once on the node thread after start(), before
//      the first message. Bootstrap a self-driving node's work loop here
//      (e.g. ::theia::runtime::post_info(*this, "tick")); leave empty for a
//      passive node. State fields live in impl/TraceCtl_state.hh.
void TraceCtl::init(TraceCtlState& /*s*/) {
}

// ---- string handle_info — the post_info()/send_after() tick path.
void TraceCtl::handle_info(const char* /*info*/, TraceCtlState& /*s*/) {
}




TraceEmpty TraceCtl::handle_call(
        const TraceConfigRequest& /*req*/,
        TraceCtlState& /*s*/) {
    // TODO: implement Configure (TraceConfigRequest →
    //                                 TraceEmpty).
    // Dispatched by request type; one or more server ports may
    // expose this operation.
    std::fprintf(stderr, "[%s] Configure called\n",
                 kNodeName);
    return TraceEmpty{};
}

// ctl_sub: an observer registers for the trace firehose. Hand it to the
// shared hub, which connects back to the observer's (sub_type,sub_instance),
// spills the ring backlog, then fans live records to it. node_filter is a
// nanopb pb_callback string (not readable here) — the observer filters by
// node itself; we pass "" (all). sub_type/sub_instance/kind are plain scalars.
TraceEmpty TraceCtl::handle_call(const SubscribeReq& req,
                                 TraceCtlState& /*s*/) {
    TraceHub::instance().subscribe(req.sub_type, req.sub_instance,
                                   req.kind, /*node_filter=*/"");
    return TraceEmpty{};
}


}  // namespace ara::log

// User handler bodies for LogDaemon.
//
// FIRST-TIME-ONLY SCAFFOLD. `artheia gen-app --kind fc` checks for
// this file's existence and refuses to overwrite unless `--force`
// is passed. Bodies are yours; the declarations are in
// lib/LogDaemon.hh.
//
// Default behaviour for every handler is a no-op (logs the message
// type to stderr so you can see traffic land). Replace with real
// behaviour as the FC matures.

#include "lib/LogDaemon.hh"

#include <cstdio>

#include "impl/log_hub.hpp"   // process-global LogHub (shared with LogStreamPump)

namespace ara::log {


// ---- OTP init/1 — runs once on the node thread after start(), before
//      the first message. Bootstrap a self-driving node's work loop here
//      (e.g. ::theia::runtime::post_info(*this, "tick")); leave empty for a
//      passive node. State fields live in impl/LogDaemon_state.hh.
void LogDaemon::init(LogDaemonState& /*s*/) {
}

// ---- string handle_info — the post_info()/send_after() tick path.
void LogDaemon::handle_info(const char* /*info*/, LogDaemonState& /*s*/) {
}

// ctl_sub: a consumer (tdb/rtdb logcat) registers for the LOG firehose. Hand it
// to the shared LogHub, which connects back to (sub_type,sub_instance), spills
// the ring backlog, then fans live lines to it. The FIRST subscriber flips the
// hub's run flag so LogStreamPump starts tailing the node log files (the exact
// paths the supervisor's GetLoggerPolicy returns); the LAST unsubscribe (a
// failed fan-out send => prune) winds the tailer back down. tag_filter is a
// fixed char[] in the pinned proto, so it's readable here; we pass it (and
// level_min) as the hub's coarse best-effort filter — the fine
// <tag-glob>:<level> DSL is applied subscriber-side.
LogEmpty LogDaemon::handle_call(const LogSubscribeReq& req,
                                LogDaemonState& /*s*/) {
    LogHub::instance().subscribe(req.sub_type, req.sub_instance,
                                 static_cast<uint32_t>(req.level_min),
                                 req.tag_filter);
    return LogEmpty{};
}


}  // namespace ara::log

// User do_* bodies for the runnable node LogStreamPump — the LOG hot path.
//
// HAND-OWNED. The pump is the log analogue of TraceStreamPump, but its
// PRODUCER is files on disk, not a TIPC submitter: while there's ≥1 subscriber
// it tails every node's log file (the exact paths the supervisor's
// GetLoggerPolicy returns), parses each new line into a LogRecord, and fans it
// out via the process-global LogHub. The atomic LogDaemon node drives the
// SAME hub from the Subscribe side; LogDaemon flips the hub's run flag on the
// first subscriber, so this loop spins up tailing on demand and idles
// otherwise — no file I/O when nobody's watching (per docs/tasks/TODO/
// log-logcat.md).
//
// GenRunnable owns the thread; do_loop() returns on stop_requested(). We poll
// LogHub::tailer_wanted() and delegate to LogHub::tail_loop() (which blocks
// tailing until the last subscriber leaves), so the thread lifecycle stays
// with the node while the tailing logic lives in the shared hub.

#include "lib/LogStreamPump.hh"

#include <chrono>
#include <cstdio>
#include <thread>

#include "impl/log_hub.hpp"   // process-global LogHub (shared with LogDaemon)
#include "MachineInstance.hh" // resolve_node_tipc — our machine-shifted addr
#include <unistd.h>           // getpid — distinct watcher instance

namespace ara::log {

void LogStreamPump::do_start() {
    std::fprintf(stderr, "[%s] log pump starting (idle until first subscriber)\n",
                 kNodeName);
}

void LogStreamPump::do_loop() {
    // OTP pg:monitor — start watching the LogRecord group ONCE. Membership drives
    // the lazy tailer: the supervisor pushes PgMembership to our addr on every
    // consumer join/leave, and tailer_wanted() reads the member count. We bind a
    // bare recv socket at our own addr for the push (a runnable has no mux
    // binding). Resolve our addr (machine-shifted instance) the same way main does.
    uint32_t self_t = 0, self_i = 0;
    ::theia::runtime::resolve_node_tipc(kNodeName, kTipcType, kTipcInstance,
                                        self_t, self_i);
    // The watcher recv socket must NOT collide with the pump's own node bind
    // (0x80010023, SEQPACKET) — an RDM PgMembership push to a name shared with a
    // SEQPACKET socket can land on the wrong one. Bind the watcher at a DISTINCT
    // instance (pid-derived, high bit set so it's never 0 / a real node instance).
    uint32_t watch_inst = (static_cast<uint32_t>(::getpid()) & 0x7FFFu) | 0x8000u;
    LogHub::instance().watch_group(kNodeName, /*binding=*/nullptr,
                                   self_t, watch_inst);

    while (!stop_requested()) {
        if (LogHub::instance().tailer_wanted()) {
            // Blocks tailing files + fanning out (broadcast_members to the watched
            // consumers) until the last consumer leaves the LogRecord group, then
            // returns here to idle — no file I/O when nobody's logcat'ing.
            LogHub::instance().tail_loop();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    std::fprintf(stderr, "[%s] log pump loop exiting\n", kNodeName);
}

void LogStreamPump::do_stop() {
    std::fprintf(stderr, "[%s] log pump stopping\n", kNodeName);
}

}  // namespace ara::log

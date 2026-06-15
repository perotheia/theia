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

namespace ara::log {

void LogStreamPump::do_start() {
    std::fprintf(stderr, "[%s] log pump starting (idle until first subscriber)\n",
                 kNodeName);
}

void LogStreamPump::do_loop() {
    while (!stop_requested()) {
        if (LogHub::instance().tailer_wanted()) {
            // Blocks tailing files + fanning out until the last subscriber
            // leaves (or stop_requested via the hub's run flag), then returns
            // here to idle.
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

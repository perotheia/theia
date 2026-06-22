// State struct for TsyncCtl — APP-OWNED.
//
// Holds the latest sync snapshot (fused from the in-process GNSS driver + the
// ptp4l backend on the tick) so GetSyncStatus serves it without re-querying, plus
// the config the node applies (poll cadence, interface, discipline gate). On
// central this node STEPS CLOCK_REALTIME from a GPS fix (clock_settime, gated by
// discipline_clock); phc2sys + ptp4l distribute it from there.

#pragma once

#include <cstdint>
#include <string>

namespace ara::tsync {

struct TsyncCtlState {
    // Latest poll result (mirrors the SyncStatus wire message).
    int         state  = 0;            // SyncState: 0=UNAVAILABLE … 3=LOCKED
    int         source = 0;            // TimeSource: 0=SYSTEM 1=PTP 2=GPS
    long long   offset_ns = 0;
    std::string grandmaster;
    std::string interface;
    std::string message = "starting";

    // Static DEPLOY params (config/tsync.json, read once in init()).
    std::string gps_serial_dev;        // "" → driver default (/dev/serial0)
    uint32_t    gps_baud = 0;          // 0 → driver default; e.g. 460800 (F9R)

    // Applied config (from TsyncConfig / on_config_update).
    std::string ptp_interface;
    uint32_t    ptp_domain = 0;
    std::string cfg_source = "ptp";   // "gps" | "ptp" | "system"
    uint32_t    poll_ms = 100;        // deploy param (config/tsync.json); 100 = 10 Hz
    uint32_t    lock_offset_ns = 100000;
    bool        discipline_clock = false;       // may step CLOCK_REALTIME from GPS
    uint32_t    gps_fix_timeout_ms = 3000;      // no fix this long → GPS HOLDOVER

    // GNSS discipline bookkeeping.
    uint64_t    last_fix_ns = 0;       // wall-clock of the last valid GPS fix
    bool        gps_disciplined = false;        // have we stepped the clock at least once
    bool        settime_eperm_logged = false;   // log the no-CAP_SYS_TIME degrade once

    // Last reported state, to detect a loss-of-lock edge (→ PHM event).
    int         last_reported_state = -1;
};

}  // namespace ara::tsync

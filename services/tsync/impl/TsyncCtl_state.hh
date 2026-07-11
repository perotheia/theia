// State struct for TsyncCtl — APP-OWNED.
//
// Holds the registered time-source table (the tsync/location inversion: sources
// register + cast observations, nothing is compiled in) and the latest sync
// snapshot (fused on the tick from the best live source, else the ptp4l
// backend) so GetSyncStatus serves it without re-querying, plus the applied
// config. On central this node STEPS CLOCK_REALTIME from the selected source's
// observation (clock_settime, gated by discipline_clock); phc2sys + ptp4l
// distribute it from there.

#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace ara::tsync {

// One registered external time source (RegisterTimeSourceReq) + its latest
// observation (TimeObservation casts). Freshness is judged against
// expected_interval_ms (3 silent intervals → stale); the observation PAIR
// (utc_ns, local_ns) projects the source's UTC to "now" without assuming zero
// cast latency.
struct RegisteredTimeSource {
    std::string kind;                       // "pps" | "nmea" | "sim" | ...
    uint32_t    priority = 0;               // highest live source wins
    uint32_t    expected_interval_ms = 1000;

    // Latest observation (all 0 until the first cast lands).
    uint64_t    utc_ns = 0;                 // source UTC at capture
    uint64_t    local_ns = 0;               // CLOCK_REALTIME at capture
    uint32_t    uncertainty_ns = 0;         // 1-sigma; 0 = unknown
    bool        pps_aligned = false;
    uint64_t    last_obs_wall_ns = 0;       // arrival time (staleness watchdog)
};

struct TsyncCtlState {
    // Latest tick result (mirrors the SyncStatus wire message).
    int         state  = 0;            // SyncState: 0=UNAVAILABLE … 3=LOCKED
    int         source = 0;            // TimeSource: 0=SYSTEM 1=PTP 2=EXTERNAL
    long long   offset_ns = 0;
    std::string grandmaster;
    std::string interface;
    std::string message = "starting";

    // The provider registry (keyed by RegisterTimeSourceReq.name).
    std::map<std::string, RegisteredTimeSource> sources;

    // Applied config (from TsyncConfig / on_config_update).
    std::string ptp_interface;
    uint32_t    ptp_domain = 0;
    std::string cfg_source = "ptp";   // "external" | "ptp" | "system"
    uint32_t    poll_ms = 100;        // deploy param (config/tsync.json); 100 = 10 Hz
    uint32_t    lock_offset_ns = 100000;
    bool        discipline_clock = false;    // may step CLOCK_REALTIME from a source
    uint32_t    source_timeout_ms = 3000;    // no fresh obs this long → drop EXTERNAL

    // Clock-discipline bookkeeping.
    uint64_t    last_obs_ns = 0;       // wall-clock of the last FRESH selected obs
    bool        ext_disciplined = false;        // stepped the clock at least once
    bool        settime_eperm_logged = false;   // log the no-CAP_SYS_TIME degrade once

    // Last reported state, to detect a loss-of-lock edge (→ PHM event).
    int         last_reported_state = -1;
};

}  // namespace ara::tsync

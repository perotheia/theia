// State struct for TsyncCtl — APP-OWNED.
//
// Holds the latest sync snapshot (polled from the PTP/chrony backend on the
// tick) so GetSyncStatus serves it without re-querying, plus the config the node
// applies (poll cadence, interface). The clock discipline lives in the kernel +
// the PTP daemons; this is just the cached status the FC reports.

#pragma once

#include <cstdint>
#include <string>

namespace ara::tsync {

struct TsyncCtlState {
    // Latest poll result (mirrors the SyncStatus wire message).
    int         state  = 0;            // SyncState: 0=UNAVAILABLE … 3=LOCKED
    int         source = 0;            // TimeSource: 0=SYSTEM 1=PTP 2=NTP
    long long   offset_ns = 0;
    std::string grandmaster;
    std::string interface;
    std::string message = "starting";

    // Applied config (from TsyncConfig / on_config_update).
    std::string ptp_interface;
    uint32_t    ptp_domain = 0;
    std::string cfg_source = "ptp";
    uint32_t    poll_ms = 1000;
    uint32_t    lock_offset_ns = 100000;

    // Last reported state, to detect a loss-of-lock edge (→ PHM event).
    int         last_reported_state = -1;
};

}  // namespace ara::tsync

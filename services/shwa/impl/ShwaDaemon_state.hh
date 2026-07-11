// State struct for ShwaDaemon — APP-OWNED, WRITE-ONCE.
//
// Holds the last telemetry reading (GetAccelStatus serves it), the applied
// ShwaConfig (poll cadence + the desired power mode), and the power-reconcile
// edge state. The backend (host or jetson, picked at build time) is stateless
// behind shwa_backend.hpp's free functions.

#pragma once

#include <cstdint>
#include <string>

#include "shwa_backend.hpp"   // AccelReading + the backend interface

namespace ara::shwa {

struct ShwaDaemonState {
    // Latest sample (the snapshot GetAccelStatus returns + the broadcast).
    AccelReading last;

    // Applied config (from ShwaConfig / on_config_update).
    uint32_t poll_ms        = 2000;
    int      power_mode     = PM_UNKNOWN;
    bool     jetson_clocks  = false;
    // NOTE: ShwaConfig.persist (persist nvpmodel across reboots) is accepted
    // on the wire but NOT implemented — apply_power_mode never consumed it,
    // so there is no state mirror. Tracked in docs/tasks (shwa persist knob).

    // Power-reconcile edge: the last mode we pushed (so we only nvpmodel on a
    // change, not every tick).
    int      applied_power_mode = -1;
    bool     started        = false;   // backend.init() done
};

}  // namespace ara::shwa

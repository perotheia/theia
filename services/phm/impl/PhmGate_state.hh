// State struct for PhmGate — APP-OWNED, WRITE-ONCE.
//
// `artheia gen-fc` seeds this once and then NEVER overwrites
// it (unless --force). PhmGate is the PHM ingest: it counts faults per
// monitored entity in the configured window and post_event()s the
// matching level-change into PhmFsm. This struct holds that per-entity
// tally + the loaded PhmConfig thresholds. The generated lib/PhmGate.hh
// #includes it and binds it as the GenServer State type.

#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "system/services/phm/phm.pb.h"   // PhmConfig + HealthLevel

namespace ara::phm {

// Per-monitored-entity fault tally. PHM does the THRESHOLD arithmetic here
// (restarts / deadlines per entity in the rolling window) and decides
// observe-vs-escalate; the FSM owns the actual OK/WARNING/DEGRADED/FAILED
// level transitions. `level` mirrors the gate's last verdict for the
// GetHealthStatus read surface (the FSM is platform-wide-worst for v1).
struct EntityFault {
    uint32_t restart_count  = 0;   // restarts seen this window
    uint32_t deadline_count = 0;   // SendTimeoutReports seen this window
    uint64_t window_start_ns = 0;  // when the current window opened
    uint64_t last_hb_seq     = 0;  // last HeartbeatReport seq (gap detect)
    bool     have_hb_seq     = false;
    // Gate's view of this entity's level. HealthLevel ordinal:
    // 0=OK 1=WARNING 2=DEGRADED 3=FAILED.
    uint32_t level = 0;
};

struct PhmGateState {
    // Per-entity fault trackers, keyed by entity name (child/worker/node).
    std::map<std::string, EntityFault> entities;

    // Loaded thresholds. Seeded with the .art PhmConfig defaults; refreshed
    // on a ConfigUpdated cast (on_config_update). Set in PhmGate::init().
    system_services_phm_PhmConfig config =
        system_services_phm_PhmConfig_init_zero;
};

}  // namespace ara::phm

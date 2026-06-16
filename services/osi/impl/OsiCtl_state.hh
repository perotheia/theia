// State struct for OsiCtl — APP-OWNED, WRITE-ONCE.
//
// Holds the cached resource snapshot (so GetResourceStatus serves it without
// re-scanning), the per-FC jiffies/timestamp BASELINE for the next CPU-delta,
// the supervisor PID (FC processes are its children), the per-FC applied limits
// (so the snapshot can echo them back), and the applied OsiConfig. No kernel
// state is owned here — it's a cache of what /proc + cgroup last reported.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "osi_backend.hpp"   // ProcSample / FcResource accounting types
#include "system/services/osi/osi.pb.h"   // the raw snapshot type (lib includes
                                          // this BEFORE it includes us, so the
                                          // `using ResourceStatus` alias isn't
                                          // visible yet — use the pb name here).

namespace ara::osi {

struct OsiCtlState {
    // The last broadcast snapshot — GetResourceStatus serves this verbatim.
    // (The lib's `ResourceStatus` alias resolves to exactly this pb type.)
    system_services_osi_ResourceStatus last_snapshot{};

    // ---- applied config (from OsiConfig / on_config_update) ----
    std::string cgroup_root = "/sys/fs/cgroup";
    std::string slice_name  = "theia.slice";
    uint32_t    poll_ms     = 2000;
    int         power_mode  = 0;      // PowerMode ordinal (.art)
    bool        jetson_clocks = false;

    // ---- CPU-delta baseline: last poll's jiffies per FC + the wall clock ----
    std::map<std::string, uint64_t> last_jiffies;
    uint64_t    last_sample_ns = 0;

    // ---- the supervisor PID (FC procs are its direct children) ----
    int         supervisor_pid = -1;
    // True once the Theia slice exists + controllers are delegated (the cgroup
    // limit-write path is available). False on an unprivileged/non-delegated
    // host — accounting still works, SetResourceLimit reports applied=false.
    bool        slice_ready = false;

    // ---- per-FC applied limits (echoed in the snapshot; persisted across
    //      polls since cgroup reads can race a just-restarted child) ----
    struct Limit { uint32_t cpu_max_pct = 0; uint64_t mem_high = 0; };
    std::map<std::string, Limit> limits;

    // ---- platform power plane ----
    bool        on_jetson = false;
    int         applied_power_mode = -1;   // last mode we pushed (edge detect)
};

}  // namespace ara::osi

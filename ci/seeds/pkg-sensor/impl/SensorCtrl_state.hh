// CI SEED — SensorCtrl state (USER-side, write-once slot of the harness's
// sensor package). Grafted before gen-fc; gen-fc skips it (the user story).
#pragma once

#include <cstdint>

namespace ara::sensor {

struct SensorCtrlState {
    uint32_t rate_ms   = 100;   // publish period (params{rate_ms}, deploy-tunable)
    uint32_t seq       = 0;     // samples published so far
};

}  // namespace ara::sensor

// CI SEED — FilterCtrl state (USER-side, write-once slot of the harness's
// filter package).
#pragma once

#include <cstdint>

namespace ara::filter {

struct FilterCtrlState {
    uint32_t received   = 0;   // samples consumed off the SampleStream group
    uint32_t last_value = 0;   // payload of the newest one
};

}  // namespace ara::filter

// State struct for DriverNode — APP-OWNED, WRITE-ONCE.
//
// `artheia gen-app --kind fc` seeds this once and then NEVER overwrites
// it (unless --force). The node's persistent state is app behaviour,
// which gen-app does NOT derive from the .art — so you own this file.
// Add the fields the node's handlers / init() need here; the generated
// lib/DriverNode.hh #includes it and binds it as the GenServer
// State type.

#pragma once

#include <cstdint>
#include <string>

namespace demo {

struct DriverNodeState {
    int  expected_value = 0;   // what the driver expects the counter to read
    int  last_value     = 0;   // last reply value observed
    int  replies_ok     = 0;
};

}  // namespace demo

// State struct for PerManager — APP-OWNED, WRITE-ONCE.
//
// `artheia gen-app --kind fc` seeds this once and then NEVER overwrites
// it (unless --force). The node's persistent state is app behaviour,
// which gen-app does NOT derive from the .art — so you own this file.
// Add the fields the node's handlers / init() need here; the generated
// lib/PerManager.hh #includes it and binds it as the GenServer
// State type.

#pragma once

#include <cstdint>
#include <string>

namespace system_services_per {

struct PerManagerState {
    // Add app fields here, e.g.:
    //   int32_t counter = 0;
};

}  // namespace system_services_per

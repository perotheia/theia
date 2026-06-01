// State struct for SupervisorCtl — APP-OWNED, WRITE-ONCE.
//
// `artheia gen-app --kind fc` seeds this once and then NEVER overwrites
// it (unless --force). The node's persistent state is app behaviour,
// which gen-app does NOT derive from the .art — so you own this file.
// Add the fields the node's handlers / init() need here; the generated
// lib/SupervisorCtl.hh #includes it and binds it as the GenServer
// State type.

#pragma once

#include <cstdint>
#include <string>

namespace ara::exec {

struct SupervisorCtlState {
    // Add app fields here, e.g.:
    //   int32_t counter = 0;
};

}  // namespace ara::exec

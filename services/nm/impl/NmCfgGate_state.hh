// State struct for NmCfgGate — APP-OWNED, WRITE-ONCE.
//
// `artheia gen-fc` seeds this once and then NEVER overwrites
// it (unless --force). The node's persistent state is app behaviour,
// which gen-fc does NOT derive from the .art — so you own this file.
// Add the fields the node's handlers / init() need here; the generated
// lib/NmCfgGate.hh #includes it and binds it as the GenServer
// State type.

#pragma once

#include <cstdint>
#include <string>

namespace ara::nm {

struct NmCfgGateState {
    // Add app fields here, e.g.:
    //   int32_t counter = 0;
};

}  // namespace ara::nm

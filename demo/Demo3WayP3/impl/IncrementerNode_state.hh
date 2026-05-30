// State struct for IncrementerNode — APP-OWNED, WRITE-ONCE.
//
// `artheia gen-app --kind fc` seeds this once and then NEVER overwrites
// it (unless --force). The node's persistent state is app behaviour,
// which gen-app does NOT derive from the .art — so you own this file.
// Add the fields the node's handlers / init() need here; the generated
// lib/IncrementerNode.hh #includes it and binds it as the GenServer
// State type.

#pragma once

#include <cstdint>
#include <string>

namespace demo {

struct IncrementerNodeState {
    int casts_sent = 0;
};

}  // namespace demo
